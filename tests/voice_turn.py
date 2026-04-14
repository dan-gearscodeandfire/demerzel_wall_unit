"""Stage 1 vertical slice — full round-trip voice turn from the ESP32.

Records N seconds of audio from the INMP441, POSTs the resulting WAV to
demerzel-voice-server on okDemerzel, receives a Piper-synthesized response
as the HTTP body, parses its WAV header to pick the right I2S sample rate,
and plays the response through the MAX98357A.

Run via `mpremote connect port:COM4 run tests/voice_turn.py`.
"""
from machine import I2S, Pin
import socket
import struct
import time
import gc

try:
    from boot import OKDEMERZEL_HOST
except ImportError:
    OKDEMERZEL_HOST = "192.168.1.156"

VOICE_PORT = 8900
PATH = "/voice_turn"

# Mic (I2S0 RX)
MIC_BCLK = 4
MIC_WS = 5
MIC_SD = 6

# Amp (I2S1 TX)
AMP_BCLK = 15
AMP_LRC = 16
AMP_DIN = 7
AMP_SD_MODE = 12

RECORD_RATE = 16000
RECORD_SECONDS = 5


# ---------- record ----------

def record_pcm16(seconds):
    audio_in = I2S(0, sck=Pin(MIC_BCLK), ws=Pin(MIC_WS), sd=Pin(MIC_SD),
                   mode=I2S.RX, bits=32, format=I2S.MONO, rate=RECORD_RATE, ibuf=16384)
    dummy = bytearray(4096)
    for _ in range(20):
        audio_in.readinto(dummy)

    rec_buf = bytearray(RECORD_RATE * 4 * seconds)
    mv = memoryview(rec_buf)
    pos = 0
    while pos < len(rec_buf):
        n = audio_in.readinto(mv[pos:])
        pos += n
    audio_in.deinit()

    # 32-bit -> 16-bit with DC removal
    n_samples = pos // 4
    dc_sum = 0
    for i in range(0, pos, 4):
        s = rec_buf[i] | (rec_buf[i+1] << 8) | (rec_buf[i+2] << 16) | (rec_buf[i+3] << 24)
        if s >= 2147483648:
            s -= 4294967296
        dc_sum += s >> 8
    dc = dc_sum // n_samples

    pcm = bytearray(n_samples * 2)
    for i in range(n_samples):
        j = i * 4
        s = rec_buf[j] | (rec_buf[j+1] << 8) | (rec_buf[j+2] << 16) | (rec_buf[j+3] << 24)
        if s >= 2147483648:
            s -= 4294967296
        s = (s >> 8) - dc
        if s > 32767:
            s = 32767
        elif s < -32768:
            s = -32768
        if s < 0:
            s += 65536
        k = i * 2
        pcm[k] = s & 0xFF
        pcm[k+1] = (s >> 8) & 0xFF
    return pcm


# ---------- WAV wrap ----------

def wrap_wav(pcm, rate=RECORD_RATE):
    ch = 1
    bps = 16
    byte_rate = rate * ch * bps // 8
    block_align = ch * bps // 8
    data_size = len(pcm)
    fmt_chunk = struct.pack("<4sIHHIIHH",
                            b"fmt ", 16, 1, ch, rate, byte_rate, block_align, bps)
    data_chunk = struct.pack("<4sI", b"data", data_size) + bytes(pcm)
    riff = struct.pack("<4sI4s", b"RIFF", 4 + len(fmt_chunk) + len(data_chunk), b"WAVE")
    return riff + fmt_chunk + data_chunk


# ---------- HTTP POST + binary response ----------

def post_voice_turn(wav_bytes):
    addr = socket.getaddrinfo(OKDEMERZEL_HOST, VOICE_PORT)[0][-1]
    s = socket.socket()
    s.settimeout(60)
    s.connect(addr)

    req = (
        "POST " + PATH + " HTTP/1.1\r\n"
        "Host: " + OKDEMERZEL_HOST + ":" + str(VOICE_PORT) + "\r\n"
        "Content-Type: audio/wav\r\n"
        "Content-Length: " + str(len(wav_bytes)) + "\r\n"
        "Connection: close\r\n\r\n"
    ).encode()
    s.send(req)

    mv = memoryview(wav_bytes)
    pos = 0
    while pos < len(mv):
        n = s.send(mv[pos:pos+2048])
        pos += n

    # Read headers
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(1024)
        if not chunk:
            break
        buf += chunk
    hdr_end = buf.find(b"\r\n\r\n") + 4
    header_bytes = buf[:hdr_end]
    body = buf[hdr_end:]

    # Parse header to get Content-Length and custom headers
    hdr_text = header_bytes.decode("latin-1", "replace")
    content_length = 0
    transcript = ""
    reply_text = ""
    latency_ms = ""
    for line in hdr_text.split("\r\n"):
        lo = line.lower()
        if lo.startswith("content-length:"):
            try:
                content_length = int(line.split(":", 1)[1].strip())
            except ValueError:
                pass
        elif lo.startswith("x-transcript:"):
            transcript = line.split(":", 1)[1].strip()
        elif lo.startswith("x-reply-text:"):
            reply_text = line.split(":", 1)[1].strip()
        elif lo.startswith("x-latency-ms:"):
            latency_ms = line.split(":", 1)[1].strip()

    # Read remaining body
    while len(body) < content_length:
        chunk = s.recv(4096)
        if not chunk:
            break
        body += chunk
    s.close()

    return body, {"transcript": transcript, "reply_text": reply_text,
                  "latency_ms": latency_ms}


# ---------- parse WAV header + play ----------

def parse_wav_header(wav):
    if wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("not a WAV")
    # Walk chunks to find fmt and data
    p = 12
    rate = 0
    ch = 1
    bps = 16
    data_start = 0
    data_size = 0
    while p < len(wav) - 8:
        chunk_id = wav[p:p+4]
        chunk_size = struct.unpack("<I", wav[p+4:p+8])[0]
        if chunk_id == b"fmt ":
            _fmt, ch, rate, _br, _ba, bps = struct.unpack("<HHIIHH", wav[p+8:p+8+16])
        elif chunk_id == b"data":
            data_start = p + 8
            data_size = chunk_size
            break
        p += 8 + chunk_size
    return rate, ch, bps, data_start, data_size


def play_wav(wav_bytes):
    rate, ch, bps, data_start, data_size = parse_wav_header(wav_bytes)
    fmt = I2S.STEREO if ch == 2 else I2S.MONO
    print("    WAV format: rate=%d ch=%d bps=%d data=%d bytes" % (rate, ch, bps, data_size))

    sd_mode = Pin(AMP_SD_MODE, Pin.OUT)
    sd_mode.value(1)
    audio_out = I2S(1, sck=Pin(AMP_BCLK), ws=Pin(AMP_LRC), sd=Pin(AMP_DIN),
                    mode=I2S.TX, bits=bps, format=fmt, rate=rate, ibuf=16384)

    mv = memoryview(wav_bytes)
    pos = data_start
    end = data_start + data_size
    while pos < end:
        n = audio_out.write(mv[pos:pos+4096])
        pos += n

    audio_out.deinit()
    sd_mode.value(0)


# ---------- main ----------

def main():
    print("=== Stage 1 voice turn ===")

    print("\n[1/4] Recording %d s from INMP441..." % RECORD_SECONDS)
    print("    >>> SPEAK NOW <<<")
    t0 = time.ticks_ms()
    pcm = record_pcm16(RECORD_SECONDS)
    print("    recorded %d bytes in %d ms" % (len(pcm), time.ticks_diff(time.ticks_ms(), t0)))

    print("\n[2/4] Wrapping WAV + POSTing to %s:%d..." % (OKDEMERZEL_HOST, VOICE_PORT))
    wav_in = wrap_wav(pcm)
    del pcm
    gc.collect()
    t1 = time.ticks_ms()
    wav_out, meta = post_voice_turn(wav_in)
    print("    round-trip %d ms, response %d bytes" % (time.ticks_diff(time.ticks_ms(), t1), len(wav_out)))
    del wav_in
    gc.collect()

    print("\n[3/4] Server metadata:")
    print("    X-Transcript: %s" % meta["transcript"])
    print("    X-Reply-Text: %s" % meta["reply_text"])
    print("    X-Latency-Ms: %s" % meta["latency_ms"])

    print("\n[4/4] Playing response...")
    play_wav(wav_out)
    print("    done.")

    print("\n=== Total wall time: %d ms ===" % time.ticks_diff(time.ticks_ms(), t0))


main()
