"""Test 2: AM312 PIR Motion Sensor on GPIO11.
Polls for 30 seconds, prints state changes.
"""
from machine import Pin
import time

PIR_PIN = 11
DURATION_S = 30
POLL_MS = 200

pir = Pin(PIR_PIN, Pin.IN)

print("=== Test 2: AM312 PIR (GPIO11) ===")
print(f"Monitoring for {DURATION_S}s — wave hand near sensor to trigger.")
print()

last_state = -1
motion_count = 0
start = time.ticks_ms()

while time.ticks_diff(time.ticks_ms(), start) < DURATION_S * 1000:
    state = pir.value()
    if state != last_state:
        elapsed = time.ticks_diff(time.ticks_ms(), start) / 1000
        if state == 1:
            motion_count += 1
            print(f"  [{elapsed:5.1f}s] MOTION detected (count: {motion_count})")
        else:
            print(f"  [{elapsed:5.1f}s] CLEAR")
        last_state = state
    time.sleep_ms(POLL_MS)

print()
if motion_count > 0:
    print(f"PASS — Detected {motion_count} motion event(s) in {DURATION_S}s.")
else:
    print(f"FAIL — No motion detected in {DURATION_S}s. Check wiring: OUT→GPIO11, VCC→3.3V, GND→GND.")
