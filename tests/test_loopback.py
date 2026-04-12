"""Test 7: Mic-to-Speaker Loopback (full-duplex I2S0 RX + I2S1 TX).
Reads INMP441 mic → DC removal + gain → MAX98357A amp for 10 seconds.
"""
from machine import I2S, Pin
import time

# Mic (I2S0 RX)
MIC_BCLK = 4
MIC_WS = 5
MIC_SD = 6

# Amp (I2S1 TX)
AMP_BCLK = 15
AMP_LRC = 16
AMP_DIN = 7
AMP_SD_MODE = 12

SAMPLE_RATE = 16000
BITS = 16
DURATION_S = 10
BUF_SIZE = 8192
CHUNK_SIZE = 512
GAIN = 8

print("=== Test 7: Mic -> Speaker Loopback ===")
print(f"  Duration: {DURATION_S}s — speak near mic, listen from speaker")
print(f"  Gain: {GAIN}x with DC offset removal")
print()

# Enable amplifier
sd_mode = Pin(AMP_SD_MODE, Pin.OUT)
sd_mode.value(1)
print("  Amplifier enabled")

audio_in = I2S(
    0,
    sck=Pin(MIC_BCLK),
    ws=Pin(MIC_WS),
    sd=Pin(MIC_SD),
    mode=I2S.RX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=BUF_SIZE,
)

audio_out = I2S(
    1,
    sck=Pin(AMP_BCLK),
    ws=Pin(AMP_LRC),
    sd=Pin(AMP_DIN),
    mode=I2S.TX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=BUF_SIZE,
)

chunk_in = bytearray(CHUNK_SIZE)
chunk_out = bytearray(CHUNK_SIZE)

# Calibrate DC offset from first few reads
print("  Calibrating DC offset...")
dc_sum = 0
dc_count = 0
for _ in range(10):
    n = audio_in.readinto(chunk_in)
    for i in range(0, n, 2):
        sample = chunk_in[i] | (chunk_in[i + 1] << 8)
        if sample >= 32768:
            sample -= 65536
        dc_sum += sample
        dc_count += 1
dc_offset = dc_sum // dc_count
print(f"  DC offset: {dc_offset}")

print("  Loopback running... speak now!")
total_bytes = 0
start = time.ticks_ms()

while time.ticks_diff(time.ticks_ms(), start) < DURATION_S * 1000:
    n = audio_in.readinto(chunk_in)
    if n > 0:
        for i in range(0, n, 2):
            sample = chunk_in[i] | (chunk_in[i + 1] << 8)
            if sample >= 32768:
                sample -= 65536
            sample = (sample - dc_offset) * GAIN
            if sample > 32767:
                sample = 32767
            if sample < -32768:
                sample = -32768
            if sample < 0:
                sample += 65536
            chunk_out[i] = sample & 0xFF
            chunk_out[i + 1] = (sample >> 8) & 0xFF
        audio_out.write(chunk_out[:n])
        total_bytes += n

elapsed_ms = time.ticks_diff(time.ticks_ms(), start)

audio_in.deinit()
audio_out.deinit()
sd_mode.value(0)
print("  Amplifier muted")

print(f"  Looped {total_bytes} bytes in {elapsed_ms}ms")
print()
if total_bytes > 0:
    print(f"PASS — Full-duplex loopback ran. Confirm you heard your voice from the speaker.")
else:
    print("FAIL — No audio data transferred. Check both mic and amp wiring.")
