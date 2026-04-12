"""Play raw PCM from /hello.raw on the MAX98357A speaker."""
from machine import I2S, Pin

AMP_BCLK = 15
AMP_LRC = 16
AMP_DIN = 7
AMP_SD_MODE = 12
SAMPLE_RATE = 16000
BITS = 16
CHUNK = 4096

print("Playing /hello.raw...")
sd_mode = Pin(AMP_SD_MODE, Pin.OUT)
sd_mode.value(1)

audio_out = I2S(
    1,
    sck=Pin(AMP_BCLK),
    ws=Pin(AMP_LRC),
    sd=Pin(AMP_DIN),
    mode=I2S.TX,
    bits=BITS,
    format=I2S.MONO,
    rate=SAMPLE_RATE,
    ibuf=16384,
)

f = open("/hello.raw", "rb")
total = 0
buf = bytearray(CHUNK)
while True:
    n = f.readinto(buf)
    if not n:
        break
    audio_out.write(buf[:n])
    total += n
f.close()

audio_out.deinit()
sd_mode.value(0)
print(f"Played {total} bytes")
