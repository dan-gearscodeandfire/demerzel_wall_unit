"""Analyze the structure of INMP441 noise to determine if it's real signal,
clock crosstalk, or random thermal noise.
"""
from machine import I2S, Pin
import math

print("=== INMP441 noise structure analysis ===")
print()

audio_in = I2S(0, sck=Pin(4), ws=Pin(5), sd=Pin(6),
               mode=I2S.RX, bits=32, format=I2S.MONO, rate=16000, ibuf=16384)

# Long warm-up
dummy = bytearray(4096)
for _ in range(20):
    audio_in.readinto(dummy)

# Capture 1 second
samples = []
target = 16000 * 4
total = 0
chunk = bytearray(1024)
while total < target:
    n = audio_in.readinto(chunk)
    if n > 0:
        for i in range(0, n, 4):
            s = chunk[i] | (chunk[i+1] << 8) | (chunk[i+2] << 16) | (chunk[i+3] << 24)
            if s >= 2147483648:
                s -= 4294967296
            samples.append(s)
        total += n
audio_in.deinit()

n = len(samples)
print("Samples:", n)

mn = min(samples)
mx = max(samples)
avg = sum(samples) // n
print("RAW 32-bit:", "avg=", avg, "min=", mn, "max=", mx)

# LSB byte distribution
lsb_counts = {}
for s in samples:
    lsb = s & 0xFF
    lsb_counts[lsb] = lsb_counts.get(lsb, 0) + 1
top_lsbs = sorted(lsb_counts.items(), key=lambda kv: -kv[1])[:5]
print("Top 5 LSB bytes:", top_lsbs)
print("(If INMP441 sends 24-bit data left-aligned, LSB should always be 0)")

# 24-bit data
data24 = [(s >> 8) for s in samples]
mn24 = min(data24)
mx24 = max(data24)
avg24 = sum(data24) // n
print("24-bit (>>8): avg=", avg24, "min=", mn24, "max=", mx24)

centered = [s - avg24 for s in data24]
num = sum(centered[i] * centered[i+1] for i in range(n - 1))
den = sum(c * c for c in centered)
acorr = num / den if den > 0 else 0
print("Autocorrelation lag-1:", round(acorr, 3))
print("(close to 1 = real audio, ~0 = random noise)")

zc = 0
for i in range(1, n):
    if (centered[i - 1] < 0) != (centered[i] < 0):
        zc += 1
print("Zero crossings:", zc, "approx freq:", zc // 2, "Hz")

# Show first 30 raw samples to look for patterns
print()
print("First 30 raw 32-bit samples:")
for i in range(30):
    print(" ", i, ":", samples[i])
