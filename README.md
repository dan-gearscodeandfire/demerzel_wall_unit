# Demerzel Wall Unit (DWU)

ESP32-S3 based voice terminal hardware for the Demerzel personal-assistant project. Mounts behind a Decora wall plate, captures voice via I2S MEMS mic, plays responses via I2S amplifier, and offloads all processing (STT / LLM / TTS) to a LAN host (`okDemerzel`).

This repository contains the firmware test scripts and bringup tooling. The full system design lives in the Demerzel project's private notes.

## Hardware

| Component | Part | Role |
|-----------|------|------|
| MCU | Lonely Binary ESP32-S3 Gold (N16R8: 16 MB flash, 8 MB OctaSPI PSRAM, IPEX antenna) | Main controller |
| Microphone | INMP441 MEMS (I2S, omnidirectional) | Voice capture |
| Amplifier | MAX98357A I2S 3 W Class D | Speaker drive |
| Speaker | 8 Ω 2 W 28 mm | Audio output |
| Fast presence | AM312 mini PIR | Instant motion wake |
| Sustained presence | LD2410C mmWave radar | Stationary occupancy |
| Environment | BME280 I2C | Temperature, humidity, pressure |

See [`docs/pinout.md`](docs/pinout.md) for the full GPIO map.

## Architecture

```
PIR / LD2410C ─┐
               ↓ (presence wake)
INMP441 → I2S0 → ESP32 → WiFi → okDemerzel
                                   │
                                   ├─ whisper.cpp  (STT)
                                   ├─ LLM
                                   └─ Piper / Kokoro (TTS)
                                   │
ESP32 ← WiFi ← raw PCM ← ─ ─ ─ ─ ─ ┘
   ↓ I2S1
MAX98357A → speaker
```

The ESP32 is intentionally kept thin: it captures audio, streams to the LAN host, plays back responses. All processing happens server-side. Whisper has been validated to handle the INMP441's raw output without any preprocessing — no HP filter, no denoising, no AGC needed.

## Software stack

Currently running **MicroPython v1.28.0** (`ESP32_GENERIC_S3-SPIRAM_OCT`) for fast iteration during bringup. Production firmware will likely move to ESP-IDF C/C++ for Opus encoding, Espressif's AFE library (AEC, AGC, NS, VAD), OTA updates, and proper deep-sleep power management.

## Repository layout

```
demerzel_wall_unit/
  tests/           # MicroPython test scripts (run via mpremote)
    test_*.py      # one-per-peripheral hardware bringup tests
    boot.py.template  # WiFi + private host config (copy → boot.py, fill in)
    play_audio.py     # play raw PCM through MAX98357A
    record_speech.py  # record 8 s of speech for STT testing
  docs/
    pinout.md      # full ESP32-S3 GPIO assignment
  tools/
    sketches/inmp441_test/  # arduino-esp32 INMP441 test (used to rule out
                            # MicroPython as the cause of a bad mic)
```

`firmware/`, `tools/arduino-cli.exe`, and `.venv/` are gitignored (large or downloadable).

## Setup

### Host tooling

```bash
python -m venv .venv
.venv/Scripts/pip install esptool mpremote
```

Download MicroPython for ESP32-S3 with octal PSRAM from
<https://micropython.org/download/ESP32_GENERIC_S3/> and place the `.bin` in `firmware/`.

### Flash MicroPython

```bash
.venv/Scripts/esptool --chip esp32s3 --port COM3 erase_flash
.venv/Scripts/esptool --chip esp32s3 --port COM3 write_flash -z 0x0 \
    firmware/ESP32_GENERIC_S3-SPIRAM_OCT-<version>.bin
```

After flashing, the COM port may re-enumerate (e.g. COM3 → COM4) because the
native USB CDC of MicroPython presents a different PID than the bootloader.

### Configure WiFi + private host info

```bash
cp tests/boot.py.template tests/boot.py
# edit tests/boot.py and fill in WIFI_SSID, WIFI_PASSWORD, OKDEMERZEL_HOST
.venv/Scripts/mpremote connect port:COM4 cp tests/boot.py :/boot.py
```

`tests/boot.py` is gitignored — your real credentials never get committed.

### Run a test

```bash
.venv/Scripts/mpremote connect port:COM4 run tests/test_neopixel.py
.venv/Scripts/mpremote connect port:COM4 run tests/test_pir.py
# ... etc
```

## Test status

All seven hardware peripherals validated end-to-end on a working board.

| # | Device | Status |
|---|--------|--------|
| 1 | WS2812 NeoPixel | PASS |
| 2 | AM312 PIR | PASS |
| 3 | BME280 (I2C) | PASS |
| 4 | LD2410C mmWave radar | PASS |
| 5 | INMP441 MEMS mic | PASS |
| 6 | MAX98357A amplifier | PASS |
| 7 | Record / playback | PASS |

A live mic-to-speaker loopback is **not** possible because the mic and
speaker are inches apart on the same board — acoustic feedback is unavoidable.
This is fine for the half-duplex wall-terminal design (listen, then speak).

End-to-end STT validated: speech captured on the ESP32, sent as raw 16-bit PCM
to whisper.cpp on the LAN host, transcribed accurately with no preprocessing.

## Notes on the INMP441

- **Use 32-bit I2S frames in MicroPython** (`bits=32`), then right-shift the
  raw sample by 8 to get the 24-bit audio. The `bits=16` mode reads only the
  least-significant 16 bits, which are zero padding — you get noise floor
  instead of audio. This is the single most important footgun on this chip.
- **L/R pin must be tied to GND or VCC**, not floating. GND selects the left
  channel (matches MicroPython mono format).
- The acoustic port is on the underside of the chip. Don't seal it.
- Noise floor is fairly high (~-27 dBFS in our setup). It does not matter
  for whisper STT — we tested it. It would matter for high-fidelity audio.

## Notes on the LD2410C wiring

The UART pin labels in the original design plan were ambiguous about
perspective. In MicroPython use:

```python
uart = UART(1, baudrate=256000, rx=Pin(17), tx=Pin(18))
```

(LD2410C TX → ESP32 GPIO17, LD2410C RX → ESP32 GPIO18.)

## License

Personal project, no license declared. If you find it useful, consider it
informational.
