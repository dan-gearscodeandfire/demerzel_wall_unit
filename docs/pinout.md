# DWU Pin Assignment — Lonely Binary ESP32-S3 Gold (N16R8)

| Peripheral | Signal | GPIO | Bus |
|-----------|--------|------|-----|
| INMP441 mic | BCLK | 4 | I2S0 |
| INMP441 mic | WS/LRCLK | 5 | I2S0 |
| INMP441 mic | SD (data in) | 6 | I2S0 |
| MAX98357A amp | BCLK | 15 | I2S1 |
| MAX98357A amp | LRC | 16 | I2S1 |
| MAX98357A amp | DIN (data out) | 7 | I2S1 |
| MAX98357A amp | SD_MODE (mute) | 12 | Digital output |
| BME280 | SDA | 9 | I2C |
| BME280 | SCL | 10 | I2C |
| LD2410C radar | LD2410C TX → ESP32 RX | 17 | UART1 (rx) |
| LD2410C radar | LD2410C RX ← ESP32 TX | 18 | UART1 (tx) |
| LD2410C radar | OUT (presence) | 8 | Digital input |
| AM312 PIR | OUT | 11 | Digital input |
| WS2812 LED | DATA (onboard) | 48 | NeoPixel |

## Connection Notes

- **COM port**: COM4 (native USB, VID 303A PID 1001) — changed from COM3 after MicroPython flash
- **MicroPython**: v1.28.0, ESP32_GENERIC_S3-SPIRAM_OCT, 7.9MB PSRAM free
- **MAC**: 10:B4:1D:CC:57:F0
