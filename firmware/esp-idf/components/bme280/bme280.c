#include "bme280.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "bme280";

#define I2C_SDA_GPIO    9
#define I2C_SCL_GPIO    10
#define I2C_FREQ_HZ     100000

#define BME280_ADDR_PRIMARY   0x76
#define BME280_ADDR_SECONDARY 0x77

/* Register addresses */
#define REG_CHIP_ID     0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_CTRL_MEAS   0xF4
#define REG_CALIB_00    0x88   /* 0x88..0xA1 (26 bytes) */
#define REG_CALIB_26    0xE1   /* 0xE1..0xE7 (7 bytes)  */
#define REG_DATA_START  0xF7   /* 8 bytes: press[3] temp[3] hum[2] */

#define CHIP_ID_BME280  0x60
#define CHIP_ID_BMP280  0x58

/* ---------- calibration data (Bosch datasheet Table 16/17) ---------- */
typedef struct {
    /* temperature */
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    /* pressure */
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    /* humidity */
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

static i2c_master_bus_handle_t  s_bus   = NULL;
static i2c_master_dev_handle_t  s_dev   = NULL;
static bme280_calib_t           s_calib;
static int32_t                  s_t_fine; /* shared between temp & press/hum */

/* ---- low-level I2C helpers ---- */

static esp_err_t bme280_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t bme280_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

/* ---- calibration ---- */

static esp_err_t bme280_read_calibration(void)
{
    uint8_t buf[26];
    esp_err_t ret;

    /* Bank 0x88..0xA1 (26 bytes): temp + pressure */
    ret = bme280_read_regs(REG_CALIB_00, buf, 26);
    if (ret != ESP_OK) return ret;

    s_calib.dig_T1 = (uint16_t)(buf[1]  << 8) | buf[0];
    s_calib.dig_T2 = (int16_t) ((buf[3]  << 8) | buf[2]);
    s_calib.dig_T3 = (int16_t) ((buf[5]  << 8) | buf[4]);

    s_calib.dig_P1 = (uint16_t)(buf[7]  << 8) | buf[6];
    s_calib.dig_P2 = (int16_t) ((buf[9]  << 8) | buf[8]);
    s_calib.dig_P3 = (int16_t) ((buf[11] << 8) | buf[10]);
    s_calib.dig_P4 = (int16_t) ((buf[13] << 8) | buf[12]);
    s_calib.dig_P5 = (int16_t) ((buf[15] << 8) | buf[14]);
    s_calib.dig_P6 = (int16_t) ((buf[17] << 8) | buf[16]);
    s_calib.dig_P7 = (int16_t) ((buf[19] << 8) | buf[18]);
    s_calib.dig_P8 = (int16_t) ((buf[21] << 8) | buf[20]);
    s_calib.dig_P9 = (int16_t) ((buf[23] << 8) | buf[22]);

    /* dig_H1 lives at 0xA1 = buf[25] */
    s_calib.dig_H1 = buf[25];

    /* Bank 0xE1..0xE7 (7 bytes): humidity */
    uint8_t hbuf[7];
    ret = bme280_read_regs(REG_CALIB_26, hbuf, 7);
    if (ret != ESP_OK) return ret;

    s_calib.dig_H2 = (int16_t) ((hbuf[1] << 8) | hbuf[0]);
    s_calib.dig_H3 = hbuf[2];
    s_calib.dig_H4 = (int16_t) ((hbuf[3] << 4) | (hbuf[4] & 0x0F));
    s_calib.dig_H5 = (int16_t) (((hbuf[4] >> 4) & 0x0F) | (hbuf[5] << 4));
    s_calib.dig_H6 = (int8_t) hbuf[6];

    return ESP_OK;
}

/* ---- compensation (Bosch datasheet section 4.2.3, int32 variant) ---- */

static float bme280_compensate_temperature(int32_t adc_T)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)s_calib.dig_T1 << 1))) *
            ((int32_t)s_calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)s_calib.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)s_calib.dig_T1))) >> 12) *
            ((int32_t)s_calib.dig_T3)) >> 14;
    s_t_fine = var1 + var2;
    int32_t T = (s_t_fine * 5 + 128) >> 8;
    return T / 100.0f;
}

static float bme280_compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)s_t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)s_calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)s_calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)s_calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_calib.dig_P3) >> 8) +
           ((var1 * (int64_t)s_calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_calib.dig_P1) >> 33;

    if (var1 == 0) return 0.0f;

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)s_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)s_calib.dig_P7) << 4);

    return (float)((uint32_t)p) / 256.0f / 100.0f; /* Pa -> hPa */
}

static float bme280_compensate_humidity(int32_t adc_H)
{
    int32_t v_x1_u32r;
    v_x1_u32r = (s_t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)s_calib.dig_H4) << 20) -
                    (((int32_t)s_calib.dig_H5) * v_x1_u32r)) +
                   ((int32_t)16384)) >> 15) *
                 (((((((v_x1_u32r * ((int32_t)s_calib.dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t)s_calib.dig_H3)) >> 11) +
                       ((int32_t)32768))) >> 10) +
                    ((int32_t)2097152)) * ((int32_t)s_calib.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                                ((int32_t)s_calib.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0) ? 0 : v_x1_u32r;
    v_x1_u32r = (v_x1_u32r > 419430400) ? 419430400 : v_x1_u32r;

    return ((uint32_t)(v_x1_u32r >> 12)) / 1024.0f;
}

/* ---- public API ---- */

esp_err_t bme280_init(void)
{
    esp_err_t ret;

    /* Create I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Try primary address 0x76, then secondary 0x77 */
    uint8_t addr = BME280_ADDR_PRIMARY;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed at 0x%02X, trying 0x%02X", addr, BME280_ADDR_SECONDARY);
        addr = BME280_ADDR_SECONDARY;
        dev_cfg.device_address = addr;
        ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add device at either address");
            return ret;
        }
    }

    /* Verify chip ID */
    uint8_t chip_id = 0;
    ret = bme280_read_regs(REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        /* If read failed on primary, try secondary address */
        if (addr == BME280_ADDR_PRIMARY) {
            ESP_LOGW(TAG, "Chip ID read failed at 0x%02X, trying 0x%02X", addr, BME280_ADDR_SECONDARY);
            i2c_master_bus_rm_device(s_dev);
            addr = BME280_ADDR_SECONDARY;
            dev_cfg.device_address = addr;
            ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
            if (ret != ESP_OK) return ret;
            ret = bme280_read_regs(REG_CHIP_ID, &chip_id, 1);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Chip ID read failed at both addresses");
                return ret;
            }
        } else {
            return ret;
        }
    }

    if (chip_id != CHIP_ID_BME280 && chip_id != CHIP_ID_BMP280) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%02X (expected 0x60 or 0x58)", chip_id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Found %s at 0x%02X",
             chip_id == CHIP_ID_BME280 ? "BME280" : "BMP280", addr);

    /* Soft reset */
    bme280_write_reg(REG_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Read calibration */
    ret = bme280_read_calibration();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration data");
        return ret;
    }

    ESP_LOGI(TAG, "Initialized successfully");
    return ESP_OK;
}

esp_err_t bme280_read(bme280_reading_t *out)
{
    if (!out || !s_dev) return ESP_ERR_INVALID_STATE;

    esp_err_t ret;

    /* Trigger forced measurement: humidity x1, temp x1, pressure x1, forced mode */
    ret = bme280_write_reg(REG_CTRL_HUM, 0x01);   /* osrs_h = x1 */
    if (ret != ESP_OK) return ret;
    ret = bme280_write_reg(REG_CTRL_MEAS, 0x25);  /* osrs_t=x1, osrs_p=x1, forced */
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(50));

    /* Read 8 bytes: press[19:12] press[11:4] press[3:0] temp[19:12] temp[11:4] temp[3:0] hum[15:8] hum[7:0] */
    uint8_t raw[8];
    ret = bme280_read_regs(REG_DATA_START, raw, 8);
    if (ret != ESP_OK) return ret;

    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);
    int32_t adc_H = ((int32_t)raw[6] << 8)  |  (int32_t)raw[7];

    /* Compensate (temperature first — sets s_t_fine for pressure & humidity) */
    out->temperature_c = bme280_compensate_temperature(adc_T);
    out->pressure_hpa  = bme280_compensate_pressure(adc_P);
    out->humidity_pct  = bme280_compensate_humidity(adc_H);

    ESP_LOGI(TAG, "T=%.1f C  H=%.1f %%  P=%.1f hPa",
             out->temperature_c, out->humidity_pct, out->pressure_hpa);

    return ESP_OK;
}
