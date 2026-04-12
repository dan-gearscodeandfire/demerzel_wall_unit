"""Test 1: Onboard WS2812 RGB LED on GPIO48.
Cycles R, G, B, White, Off to confirm basic GPIO and NeoPixel driver.
"""
import neopixel
from machine import Pin
import time

LED_PIN = 48
NUM_LEDS = 1

np = neopixel.NeoPixel(Pin(LED_PIN), NUM_LEDS)

colors = [
    ("RED",   (255, 0, 0)),
    ("GREEN", (0, 255, 0)),
    ("BLUE",  (0, 0, 255)),
    ("WHITE", (255, 255, 255)),
    ("OFF",   (0, 0, 0)),
]

print("=== Test 1: NeoPixel (GPIO48) ===")
for name, rgb in colors:
    np[0] = rgb
    np.write()
    print(f"  {name}: {rgb}")
    time.sleep(0.5)

print("PASS — LED cycled through 5 states. Visually confirm color changes.")
