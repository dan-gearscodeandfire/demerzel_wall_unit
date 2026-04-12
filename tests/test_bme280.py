"""Test 3: BME280 Environment Sensor on I2C (SDA=GPIO9, SCL=GPIO10).
Scans I2C bus, reads temperature, humidity, pressure.
"""
from machine import I2C, Pin
import time

SDA_PIN = 9
SCL_PIN = 10

print("=== Test 3: BME280 (I2C SDA=GPIO9, SCL=GPIO10) ===")

i2c = I2C(0, sda=Pin(SDA_PIN), scl=Pin(SCL_PIN), freq=100000)

# Step 1: Bus scan
devices = i2c.scan()
print(f"  I2C scan: {[hex(d) for d in devices]}")

if not devices:
    print("FAIL — No I2C devices found. Check wiring: SDA→GPIO9, SCL→GPIO10, VCC→3.3V, GND→GND.")
    raise SystemExit

bme_addr = None
for addr in (0x76, 0x77):
    if addr in devices:
        bme_addr = addr
        break

if bme_addr is None:
    print(f"FAIL — BME280 not found at 0x76 or 0x77. Found: {[hex(d) for d in devices]}")
    raise SystemExit

print(f"  BME280 found at {hex(bme_addr)}")

# Step 2: Read chip ID (register 0xD0, should be 0x60 for BME280)
chip_id = i2c.readfrom_mem(bme_addr, 0xD0, 1)[0]
print(f"  Chip ID: 0x{chip_id:02x} (expected 0x60 for BME280, 0x58 for BMP280)")

if chip_id not in (0x60, 0x58):
    print(f"FAIL — Unexpected chip ID 0x{chip_id:02x}.")
    raise SystemExit

# Step 3: Minimal raw read — force a measurement
# Write ctrl_hum (0xF2) = 0x01 (1x oversampling humidity)
i2c.writeto_mem(bme_addr, 0xF2, bytes([0x01]))
# Write ctrl_meas (0xF4) = 0x25 (1x temp, 1x pressure, forced mode)
i2c.writeto_mem(bme_addr, 0xF4, bytes([0x25]))
time.sleep_ms(50)

# Read raw data (0xF7..0xFE = 8 bytes: press[3], temp[3], hum[2])
raw = i2c.readfrom_mem(bme_addr, 0xF7, 8)
raw_press = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4)
raw_temp = (raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4)
raw_hum = (raw[6] << 8) | raw[7]

print(f"  Raw values — temp: {raw_temp}, press: {raw_press}, hum: {raw_hum}")

# Step 4: Read calibration and compute compensated values
# Temperature calibration (0x88..0x8D)
cal = i2c.readfrom_mem(bme_addr, 0x88, 6)
dig_T1 = cal[0] | (cal[1] << 8)
dig_T2 = cal[2] | (cal[3] << 8)
if dig_T2 >= 32768: dig_T2 -= 65536
dig_T3 = cal[4] | (cal[5] << 8)
if dig_T3 >= 32768: dig_T3 -= 65536

# Compensate temperature (BME280 datasheet formula)
var1 = ((raw_temp / 16384.0) - (dig_T1 / 1024.0)) * dig_T2
var2 = (((raw_temp / 131072.0) - (dig_T1 / 8192.0)) ** 2) * dig_T3
t_fine = var1 + var2
temp_c = t_fine / 5120.0
temp_f = temp_c * 9 / 5 + 32

print(f"  Temperature: {temp_c:.1f} C / {temp_f:.1f} F")

# Sanity check
if -40 < temp_c < 85:
    print(f"PASS — BME280 responding, temperature {temp_c:.1f}C is plausible.")
else:
    print(f"FAIL — Temperature {temp_c:.1f}C is outside plausible range (-40..85C).")
