"""Test 4: LD2410C mmWave Radar on UART1 (TX=GPIO17, RX=GPIO18) + OUT=GPIO8.
Reads digital presence pin and parses UART frames for 30 seconds.
"""
from machine import UART, Pin
import time

UART_TX = 18  # ESP32 TX (GPIO18) → LD2410C RX
UART_RX = 17  # ESP32 RX (GPIO17) ← LD2410C TX
OUT_PIN = 8
DURATION_S = 30

# LD2410C protocol constants
HEADER = bytes([0xF4, 0xF3, 0xF2, 0xF1])
FOOTER = bytes([0xF8, 0xF7, 0xF6, 0xF5])

print("=== Test 4: LD2410C mmWave Radar ===")

# Step 1: Digital OUT pin
out = Pin(OUT_PIN, Pin.IN)
print(f"  GPIO{OUT_PIN} (OUT) initial: {'PRESENCE' if out.value() else 'NO PRESENCE'}")

# Step 2: UART at 256000 baud
uart = UART(1, baudrate=256000, tx=Pin(UART_TX), rx=Pin(UART_RX))
print(f"  UART1 opened at 256000 baud (TX=GPIO{UART_TX}, RX=GPIO{UART_RX})")
print(f"  Monitoring for {DURATION_S}s — walk near/away from sensor.")
print()

frame_count = 0
presence_changes = 0
last_presence = -1
start = time.ticks_ms()
buf = bytearray(64)

while time.ticks_diff(time.ticks_ms(), start) < DURATION_S * 1000:
    # Check digital OUT pin for changes
    cur_presence = out.value()
    if cur_presence != last_presence:
        elapsed = time.ticks_diff(time.ticks_ms(), start) / 1000
        state = "PRESENCE" if cur_presence else "NO PRESENCE"
        print(f"  [{elapsed:5.1f}s] OUT pin: {state}")
        if last_presence != -1:
            presence_changes += 1
        last_presence = cur_presence

    # Try to read UART frames
    avail = uart.any()
    if avail > 0:
        data = uart.read(min(avail, 64))
        if data:
            # Search for header bytes
            hdr_idx = -1
            for i in range(len(data) - 3):
                if data[i] == 0xF4 and data[i+1] == 0xF3 and data[i+2] == 0xF2 and data[i+3] == 0xF1:
                    hdr_idx = i
                    break
            if hdr_idx >= 0:
                frame_count += 1
                # Byte at offset +8 from header: target state
                si = hdr_idx + 8
                if si < len(data):
                    target_state = data[si]
                    states = {0: "no target", 1: "moving", 2: "stationary", 3: "moving+stationary"}
                    state_str = states.get(target_state, "unknown(%d)" % target_state)
                    if frame_count % 20 == 1:
                        elapsed = time.ticks_diff(time.ticks_ms(), start) / 1000
                        print("  [%5.1fs] Frame #%d: %s" % (elapsed, frame_count, state_str))

    time.sleep_ms(50)

print()
print(f"  Total UART frames received: {frame_count}")
print(f"  Presence state changes (OUT pin): {presence_changes}")
print()
if frame_count > 0:
    print(f"PASS — LD2410C communicating. {frame_count} frames, {presence_changes} presence changes.")
else:
    if presence_changes > 0:
        print(f"PARTIAL PASS — OUT pin working ({presence_changes} changes) but no UART frames.")
        print("  Check UART wiring: ESP32 GPIO17(TX)→LD2410C RX, ESP32 GPIO18(RX)←LD2410C TX")
    else:
        print("FAIL — No UART frames and no OUT pin changes.")
        print("  Check power (5V) and all wiring.")
