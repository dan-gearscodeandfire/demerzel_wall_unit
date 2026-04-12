"""Test 6: MAX98357A Amplifier on I2S1 (BCLK=GPIO15, LRC=GPIO16, DIN=GPIO7, SD_MODE=GPIO12).
Generates and plays a 440Hz sine tone for 2 seconds.
"""
from machine import I2S, Pin
import math
import time

BCLK_PIN = 15
LRC_PIN = 16
DIN_PIN = 7
SD_MODE_PIN = 12
SAMPLE_RATE = 16000
BITS = 16
DURATION_S = 2
TONE_HZ = 440
VOLUME = 8000  # amplitude (max 32767)
BUF_SIZE = 8192

print("=== Test 6: MAX98357A Amplifier (I2S1) ===")
print(f"  Config: {SAMPLE_RATE}Hz, {BITS}-bit, mono")
print(f"  Pins: BCLK=GPIO{BCLK_PIN}, LRC=GPIO{LRC_PIN}, DIN=GPIO{DIN_PIN}, SD_MODE=GPIO{SD_MODE_PIN}")
print()

# Enable amplifier
sd_mode = Pin(SD_MODE_PIN, Pin.OUT)
sd_mode.value(1)
print("  SD_MODE HIGH — amplifier enabled")

audio_out = I2S(
    1,
    sck=Pin(BCLK_PIN),
    ws=Pin(LRC_PIN),
    sd=Pin(DIN_PIN),
    mode=I2S.TX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=BUF_SIZE,
)

# Pre-generate one period of sine wave
samples_per_period = SAMPLE_RATE // TONE_HZ
period_buf = bytearray(samples_per_period * 2)
for i in range(samples_per_period):
    val = int(VOLUME * math.sin(2 * math.pi * i / samples_per_period))
    # 16-bit signed little-endian
    period_buf[i * 2] = val & 0xFF
    period_buf[i * 2 + 1] = (val >> 8) & 0xFF

total_bytes = SAMPLE_RATE * 2 * DURATION_S
bytes_sent = 0

print(f"  Playing {TONE_HZ}Hz tone for {DURATION_S}s...")
start = time.ticks_ms()

while bytes_sent < total_bytes:
    num_written = audio_out.write(period_buf)
    bytes_sent += num_written

elapsed_ms = time.ticks_diff(time.ticks_ms(), start)

audio_out.deinit()

# Mute
sd_mode.value(0)
print("  SD_MODE LOW — amplifier muted")

print(f"  Sent {bytes_sent} bytes in {elapsed_ms}ms")
print()
print(f"PASS — 440Hz tone played for ~{DURATION_S}s. Confirm you heard it from the speaker.")
