// INMP441 test using ESP-IDF I2S driver directly via arduino-esp32.
// Bypasses MicroPython entirely to determine if the mic works at all.
//
// Wiring (matches DWU plan):
//   INMP441 BCLK -> GPIO4
//   INMP441 WS   -> GPIO5
//   INMP441 SD   -> GPIO6
//   INMP441 VDD  -> 3V3
//   INMP441 GND  -> GND
//   INMP441 L/R  -> GND (left channel)

#include <Arduino.h>
#include "driver/i2s.h"
#include <math.h>

#define I2S_PORT     I2S_NUM_0
#define I2S_BCLK_PIN 4
#define I2S_WS_PIN   5
#define I2S_SD_PIN   6

#define SAMPLE_RATE  16000
#define BUF_SAMPLES  1024
int32_t i2s_buf[BUF_SAMPLES];

void setup_i2s() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_BCLK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD_PIN
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

void analyze(const char* label, uint32_t windows) {
  Serial.printf("--- %s (%lu windows of %d samples) ---\n", label, (unsigned long)windows, BUF_SAMPLES);
  for (uint32_t w = 0; w < windows; w++) {
    size_t bytes_read = 0;
    i2s_read(I2S_PORT, i2s_buf, sizeof(i2s_buf), &bytes_read, portMAX_DELAY);
    int n = bytes_read / 4;
    if (n == 0) { Serial.println("  no data"); continue; }

    // Shift right 8 to extract 24-bit data into 32-bit signed range
    long long sum = 0;
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    for (int i = 0; i < n; i++) {
      int32_t s = i2s_buf[i] >> 8;
      sum += s;
      if (s < mn) mn = s;
      if (s > mx) mx = s;
    }
    int32_t avg = (int32_t)(sum / n);
    double sumsq = 0;
    for (int i = 0; i < n; i++) {
      int32_t s = (i2s_buf[i] >> 8) - avg;
      sumsq += (double)s * s;
    }
    double rms = sqrt(sumsq / n);
    Serial.printf("  win %2lu: avg=%8ld rms=%9.0f range=[%9ld, %9ld]\n",
                  (unsigned long)w, (long)avg, rms, (long)mn, (long)mx);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=================================");
  Serial.println("  INMP441 Arduino-ESP32 Test");
  Serial.println("=================================");
  Serial.println();

  setup_i2s();

  // Warm up
  size_t br;
  for (int i = 0; i < 10; i++) {
    i2s_read(I2S_PORT, i2s_buf, sizeof(i2s_buf), &br, portMAX_DELAY);
  }

  Serial.println("Phase 1: BE QUIET (10 windows ~ 0.6s baseline)");
  Serial.println("  ...recording in 3s...");
  delay(3000);
  analyze("QUIET", 10);

  Serial.println();
  Serial.println("Phase 2: MAKE NOISE - whistle/clap LOUDLY (10 windows)");
  Serial.println("  ...starts in 3s...");
  delay(3000);
  Serial.println("  >>> NOISE NOW <<<");
  analyze("NOISY", 10);

  Serial.println();
  Serial.println("=== Done. Compare RMS between phases. ===");
}

void loop() {
  // Continuous monitor mode after the two-phase test
  size_t br;
  i2s_read(I2S_PORT, i2s_buf, sizeof(i2s_buf), &br, portMAX_DELAY);
  int n = br / 4;
  if (n > 0) {
    long long sum = 0;
    for (int i = 0; i < n; i++) sum += (i2s_buf[i] >> 8);
    int32_t avg = (int32_t)(sum / n);
    double sumsq = 0;
    for (int i = 0; i < n; i++) {
      int32_t s = (i2s_buf[i] >> 8) - avg;
      sumsq += (double)s * s;
    }
    double rms = sqrt(sumsq / n);
    int bars = (int)(rms / 200);
    if (bars > 60) bars = 60;
    Serial.printf("RMS=%6.0f ", rms);
    for (int i = 0; i < bars; i++) Serial.print('#');
    Serial.println();
  }
  delay(50);
}
