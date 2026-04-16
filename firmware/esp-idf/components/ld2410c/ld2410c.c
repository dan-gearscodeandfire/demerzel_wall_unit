#include "ld2410c.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ld2410c";

#define UART_NUM        UART_NUM_1
#define UART_BAUD       256000
#define UART_RX_GPIO    17
#define UART_TX_GPIO    18
#define OUT_PIN_GPIO    8

#define UART_BUF_SIZE   256
#define TASK_STACK_SIZE 3072

/* Frame markers */
static const uint8_t HEADER[] = { 0xF4, 0xF3, 0xF2, 0xF1 };
static const uint8_t FOOTER[] = { 0xF8, 0xF7, 0xF6, 0xF5 };

static SemaphoreHandle_t s_mutex    = NULL;
static TaskHandle_t      s_task     = NULL;
static bool              s_running  = false;

/* Protected by s_mutex */
static uint8_t s_target_state = 0;
static bool    s_uart_valid   = false;

/* ---- frame parser ---- */

static void ld2410c_parse_task(void *arg)
{
    uint8_t buf[UART_BUF_SIZE];
    int head = 0;  /* bytes in buf waiting to be consumed */

    while (s_running) {
        /* Read more data from UART, appending after existing head bytes */
        int avail = UART_BUF_SIZE - head;
        if (avail <= 0) {
            /* Buffer full with no valid frame — discard and resync */
            head = 0;
            continue;
        }
        int len = uart_read_bytes(UART_NUM, buf + head, avail, pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        head += len;

        /* Scan for frames in the buffer */
        int pos = 0;
        while (pos + 4 <= head) {
            /* Look for header */
            if (memcmp(buf + pos, HEADER, 4) != 0) {
                pos++;
                continue;
            }

            /* Need at least header(4) + type(1) + head(1) + data_len(2) = 8 bytes to read data_len */
            if (pos + 8 > head) break; /* wait for more data */

            uint16_t data_len = (uint16_t)buf[pos + 6] | ((uint16_t)buf[pos + 7] << 8);

            /* Full frame: header(4) + type(1) + head(1) + data_len(2) + data(data_len) + footer(4) */
            int frame_len = 4 + 1 + 1 + 2 + data_len + 4;
            if (pos + frame_len > head) break; /* incomplete frame */

            /* Verify footer */
            if (memcmp(buf + pos + frame_len - 4, FOOTER, 4) == 0) {
                /* Target state byte is at offset 8 within the frame (header+4, after type+head+data_len) */
                uint8_t target = buf[pos + 8];

                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_target_state = target;
                s_uart_valid   = true;
                xSemaphoreGive(s_mutex);
            }

            pos += frame_len;
        }

        /* Shift unconsumed bytes to front */
        if (pos > 0 && pos < head) {
            memmove(buf, buf + pos, head - pos);
        }
        head -= pos;
    }

    vTaskDelete(NULL);
}

/* ---- public API ---- */

esp_err_t ld2410c_init(void)
{
    esp_err_t ret;

    /* Configure UART */
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ret = uart_param_config(UART_NUM, &uart_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(UART_NUM, UART_TX_GPIO, UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    /* Configure OUT pin as input */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << OUT_PIN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) return ret;

    /* Create mutex and start parse task */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_running = true;
    BaseType_t xret = xTaskCreate(ld2410c_parse_task, "ld2410c", TASK_STACK_SIZE,
                                  NULL, 5, &s_task);
    if (xret != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized: UART%d %d baud, RX=GPIO%d, TX=GPIO%d, OUT=GPIO%d",
             UART_NUM, UART_BAUD, UART_RX_GPIO, UART_TX_GPIO, OUT_PIN_GPIO);
    return ESP_OK;
}

esp_err_t ld2410c_get_state(ld2410c_state_t *out)
{
    if (!out || !s_mutex) return ESP_ERR_INVALID_STATE;

    /* Read GPIO OUT pin directly */
    out->presence = (gpio_get_level(OUT_PIN_GPIO) == 1);

    /* Read UART-parsed state under mutex */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->target_state = s_target_state;
    out->uart_valid   = s_uart_valid;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

void ld2410c_deinit(void)
{
    s_running = false;
    if (s_task) {
        /* Give the task time to exit */
        vTaskDelay(pdMS_TO_TICKS(200));
        s_task = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    uart_driver_delete(UART_NUM);
    ESP_LOGI(TAG, "Deinitialized");
}
