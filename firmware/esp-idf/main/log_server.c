#include "log_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "log_server";

#define MAX_CLIENTS 3

static int s_client_fds[MAX_CLIENTS];
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t s_original_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    // Always write to UART first
    int ret = s_original_vprintf(fmt, args);

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        char buf[512];
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        va_end(args_copy);

        if (len > 0) {
            if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (s_client_fds[i] >= 0) {
                    int sent = send(s_client_fds[i], buf, len, MSG_DONTWAIT);
                    if (sent < 0) {
                        close(s_client_fds[i]);
                        s_client_fds[i] = -1;
                    }
                }
            }
        }
        xSemaphoreGive(s_mutex);
    }

    return ret;
}

static void log_server_task(void *param)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_DWU_LOG_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed on port %d", CONFIG_DWU_LOG_TCP_PORT);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 2) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP log server listening on port %d", CONFIG_DWU_LOG_TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            bool placed = false;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (s_client_fds[i] < 0) {
                    s_client_fds[i] = client_fd;
                    placed = true;
                    ESP_LOGI(TAG, "Log client connected (slot %d)", i);
                    break;
                }
            }
            xSemaphoreGive(s_mutex);

            if (!placed) {
                const char *msg = "Max log clients reached\r\n";
                send(client_fd, msg, strlen(msg), 0);
                close(client_fd);
            }
        }
    }
}

esp_err_t log_server_init(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_client_fds[i] = -1;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    xTaskCreate(log_server_task, "log_server", 4096, NULL, 2, NULL);

    return ESP_OK;
}
