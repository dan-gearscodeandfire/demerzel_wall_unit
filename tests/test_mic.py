"""Test 5: INMP441 MEMS Microphone on I2S0 (BCLK=GPIO4, WS=GPIO5, SD=GPIO6).
Captures 3 seconds of audio, computes signal stats, saves raw PCM.
"""
from machine import I2S, Pin
import math
import time

BCLK_PIN = 4
WS_PIN = 5
SD_PIN = 6
SAMPLE_RATE = 16000
BITS = 16
DURATION_S = 3
BUF_SIZE = 8192  # I2S DMA buffer

print("=== Test 5: INMP441 MEMS Microphone (I2S0) ===")
print(f"  Config: {SAMPLE_RATE}Hz, {BITS}-bit, mono")
print(f"  Pins: BCLK=GPIO{BCLK_PIN}, WS=GPIO{WS_PIN}, SD=GPIO{SD_PIN}")
print()

audio_in = I2S(
    0,
    sck=Pin(BCLK_PIN),
    ws=Pin(WS_PIN),
    sd=Pin(SD_PIN),
    mode=I2S.RX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=BUF_SIZE,
)

total_samples = SAMPLE_RATE * DURATION_S
bytes_per_sample = BITS // 8
total_bytes = total_samples * bytes_per_sample
chunk = bytearray(1024)

print(f"  Recording {DURATION_S}s ({total_bytes} bytes)...")

# Capture to file
f = open("/recording.raw", "wb")
bytes_written = 0
sample_min = 32767
sample_max = -32768
sum_sq = 0
sample_count = 0

start = time.ticks_ms()
while bytes_written < total_bytes:
    num_read = audio_in.readinto(chunk)
    if num_read > 0:
        f.write(chunk[:num_read])
        bytes_written += num_read
        # Compute stats from 16-bit signed samples
        for i in range(0, num_read, 2):
            if i + 1 < num_read:
                sample = chunk[i] | (chunk[i + 1] << 8)
                if sample >= 32768:
                    sample -= 65536
                if sample < sample_min:
                    sample_min = sample
                if sample > sample_max:
                    sample_max = sample
                sum_sq += sample * sample
                sample_count += 1

elapsed_ms = time.ticks_diff(time.ticks_ms(), start)
f.close()
audio_in.deinit()

rms = math.sqrt(sum_sq / sample_count) if sample_count > 0 else 0

print(f"  Captured {bytes_written} bytes in {elapsed_ms}ms")
print(f"  Samples: {sample_count}")
print(f"  Min: {sample_min}, Max: {sample_max}, Peak-to-peak: {sample_max - sample_min}")
print(f"  RMS: {rms:.1f}")
print(f"  Saved to /recording.raw (16-bit signed LE, mono, {SAMPLE_RATE}Hz)")
print()

# Evaluate
if sample_count == 0:
    print("FAIL — No samples captured. Check wiring: BCLK→GPIO4, WS→GPIO5, SD→GPIO6, VCC→3.3V, GND→GND.")
elif sample_max - sample_min < 10:
    print("FAIL — Signal is flat (peak-to-peak < 10). Mic may not be connected or L/R select is wrong.")
    print("  INMP441 L/R pin: GND=left channel, VCC=right channel. Try the other.")
elif rms > 30000:
    print("FAIL — Signal is clipping (RMS > 30000). Check for wiring noise or shorts.")
else:
    print(f"PASS — Mic captured audio. RMS={rms:.1f}, range=[{sample_min}, {sample_max}].")
    print("  To verify on PC: mpremote connect port:COM4 cp :/recording.raw recording.raw")
    print("  Then open in Audacity: File → Import Raw → 16-bit signed, Little-endian, Mono, 16000Hz")
