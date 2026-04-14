"""Record N seconds of audio on the INMP441, POST it to whisper.cpp on
okDemerzel as a multipart WAV upload, and print the transcription.

Designed to be run via `mpremote run`. The PC reads the printed output
to extract the transcription. The MARKER lines bracket the transcription
so the host parser can find it deterministically.
"""
from machine import I2S, Pin
import socket
import time
import struct

try:
    from boot import OKDEMERZEL_HOST, WHISPER_PORT
except ImportError:
    OKDEMERZEL_HOST = "192.168.1.156"
    WHISPER_PORT = 8891

RATE = 16000
SECONDS = 5  # adjust per call by editing this constant or pass via stdin

# ---------- record ----------

def record_pcm(seconds):
    audio_in = I2S(0, sck=Pin(4), ws=Pin(5), sd=Pin(6),
                   mode=I2S.RX, bits=32, format=I2S.MONO, rate=RATE, ibuf=16384)
    dummy = bytearray(4096)
    for _ in range(20):
        audio_in.readinto(dummy)

    rec_buf = bytearray(RATE * 4 * seconds)
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

# ---------- WAV wrapping ----------

def wrap_wav(pcm, rate=RATE):
    """Wrap raw 16-bit mono PCM in a WAV header."""
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

# ---------- HTTP multipart POST ----------

def post_to_whisper(wav_bytes):
    boundary = "----dwuboundary7fG3kP"
    head = (
        '--' + boundary + '\r\n'
        'Content-Disposition: form-data; name="file"; filename="speech.wav"\r\n'
        'Content-Type: audio/wav\r\n\r\n'
    ).encode()
    tail = (
        '\r\n--' + boundary + '\r\n'
        'Content-Disposition: form-data; name="response_format"\r\n\r\n'
        'json\r\n'
        '--' + boundary + '--\r\n'
    ).encode()

    body_len = len(head) + len(wav_bytes) + len(tail)
    req = (
        'POST /inference HTTP/1.1\r\n'
        'Host: ' + OKDEMERZEL_HOST + ':' + str(WHISPER_PORT) + '\r\n'
        'Content-Type: multipart/form-data; boundary=' + boundary + '\r\n'
        'Content-Length: ' + str(body_len) + '\r\n'
        'Connection: close\r\n\r\n'
    ).encode()

    addr = socket.getaddrinfo(OKDEMERZEL_HOST, WHISPER_PORT)[0][-1]
    s = socket.socket()
    s.settimeout(30)
    s.connect(addr)
    s.send(req)
    s.send(head)
    # Stream the WAV in chunks to avoid blowing the TX buffer
    view = memoryview(wav_bytes)
    pos = 0
    chunk = 2048
    while pos < len(view):
        n = s.send(view[pos:pos+chunk])
        pos += n
    s.send(tail)

    # Read full response
    resp = b""
    while True:
        try:
            data = s.recv(2048)
        except OSError:
            break
        if not data:
            break
        resp += data
    s.close()
    return resp

def parse_response(resp):
    # Split header / body
    idx = resp.find(b"\r\n\r\n")
    if idx < 0:
        return None
    body = resp[idx+4:].decode("utf-8", "replace").strip()
    # whisper.cpp returns {"text":"..."}
    if '"text"' in body:
        i = body.find('"text"')
        i = body.find(':', i) + 1
        # find first quoted string after the colon
        while i < len(body) and body[i] in ' \t':
            i += 1
        if i < len(body) and body[i] == '"':
            i += 1
            j = i
            out = []
            while j < len(body):
                c = body[j]
                if c == '\\' and j+1 < len(body):
                    nxt = body[j+1]
                    if nxt == 'n':
                        out.append('\n')
                    elif nxt == 't':
                        out.append('\t')
                    elif nxt == '"':
                        out.append('"')
                    elif nxt == '\\':
                        out.append('\\')
                    else:
                        out.append(nxt)
                    j += 2
                elif c == '"':
                    return ''.join(out).strip()
                else:
                    out.append(c)
                    j += 1
    return body

# ---------- main ----------

print("DWU-STT-START")
print("Recording", SECONDS, "s of audio...")
t0 = time.ticks_ms()
pcm = record_pcm(SECONDS)
t1 = time.ticks_ms()
print("Recorded in", time.ticks_diff(t1, t0), "ms,", len(pcm), "bytes PCM")

wav = wrap_wav(pcm)
print("WAV wrapped:", len(wav), "bytes")

print("POSTing to whisper at", OKDEMERZEL_HOST + ":" + str(WHISPER_PORT), "...")
t2 = time.ticks_ms()
resp = post_to_whisper(wav)
t3 = time.ticks_ms()
print("Whisper responded in", time.ticks_diff(t3, t2), "ms,", len(resp), "bytes")

text = parse_response(resp)
print("DWU-STT-TEXT:", repr(text))
print("DWU-STT-END")
