"""Record 3 seconds of speech for wake-word dataset capture.

Forked from record_speech.py with a shorter buffer — 3 s instead of 8 s —
so mpremote cp transfers stay under ~40 s per clip during the 24-clip
GUI prompter (tests/gui_record_wakeword.py). Saves to /speech.raw on
ESP32 flash, same filename/layout as record_speech.py.
"""
from machine import I2S, Pin
import time

DURATION_S = 3

audio_in = I2S(0, sck=Pin(4), ws=Pin(5), sd=Pin(6),
               mode=I2S.RX, bits=32, format=I2S.MONO, rate=16000, ibuf=16384)
dummy = bytearray(4096)
for _ in range(20):
    audio_in.readinto(dummy)

print("=== Recording %ds of speech ===" % DURATION_S)
print("Recording in 3...")
time.sleep(1)
print("            2...")
time.sleep(1)
print("            1...")
time.sleep(1)
print(">>> SPEAK NOW (%d seconds) <<<" % DURATION_S)

rec_buf = bytearray(16000 * 4 * DURATION_S)
mv = memoryview(rec_buf)
pos = 0
while pos < len(rec_buf):
    n = audio_in.readinto(mv[pos:])
    pos += n
audio_in.deinit()
print("Done recording, converting...")

# Convert 32-bit -> 16-bit with DC removal
n_samples = pos // 4
dc_sum = 0
for i in range(0, pos, 4):
    s = rec_buf[i] | (rec_buf[i+1] << 8) | (rec_buf[i+2] << 16) | (rec_buf[i+3] << 24)
    if s >= 2147483648:
        s -= 4294967296
    dc_sum += s >> 8
dc = dc_sum // n_samples

buf16 = bytearray(n_samples * 2)
clipped = 0
for i in range(n_samples):
    j = i * 4
    s = rec_buf[j] | (rec_buf[j + 1] << 8) | (rec_buf[j + 2] << 16) | (rec_buf[j + 3] << 24)
    if s >= 2147483648:
        s -= 4294967296
    s = (s >> 8) - dc
    if s > 32767:
        s = 32767
        clipped += 1
    elif s < -32768:
        s = -32768
        clipped += 1
    if s < 0:
        s += 65536
    k = i * 2
    buf16[k] = s & 0xFF
    buf16[k + 1] = (s >> 8) & 0xFF

f = open("/speech.raw", "wb")
f.write(buf16)
f.close()
print("Saved /speech.raw size=%d  DC=%d  clipped=%d" % (len(buf16), dc, clipped))
