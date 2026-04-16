#include "pir.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "pir";

#define PIR_GPIO        11
#define DEBOUNCE_MS     200
#define TASK_STACK_SIZE 2048

static QueueHandle_t   s_evt_queue = NULL;
static pir_callback_t  s_callback  = NULL;

static void IRAM_ATTR pir_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_evt_queue, &gpio_num, NULL);
}

static void pir_task(void *arg)
{
    uint32_t io_num;
    TickType_t last_event_tick = 0;

    while (1) {
        if (xQueueReceive(s_evt_queue, &io_num, portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();
            TickType_t debounce_ticks = pdMS_TO_TICKS(DEBOUNCE_MS);

            if ((now - last_event_tick) >= debounce_ticks) {
                last_event_tick = now;
                bool level = (gpio_get_level(PIR_GPIO) == 1);
                ESP_LOGD(TAG, "Motion %s", level ? "detected" : "cleared");
                if (s_callback) {
                    s_callback(level);
                }
            }
        }
    }
}

esp_err_t pir_init(pir_callback_t cb)
{
    esp_err_t ret;
    s_callback = cb;

    /* Configure GPIO as input, no pull */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) return ret;

    /* Create event queue */
    s_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    /* Install ISR service and hook handler */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means ISR service already installed — that's fine */
        return ret;
    }

    ret = gpio_isr_handler_add(PIR_GPIO, pir_isr_handler, (void *)PIR_GPIO);
    if (ret != ESP_OK) return ret;

    /* Start debounce/callback task */
    BaseType_t xret = xTaskCreate(pir_task, "pir", TASK_STACK_SIZE, NULL, 5, NULL);
    if (xret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "AM312 PIR initialized on GPIO%d (any-edge, %dms debounce)",
             PIR_GPIO, DEBOUNCE_MS);
    return ESP_OK;
}

bool pir_get_state(void)
{
    return (gpio_get_level(PIR_GPIO) == 1);
}
