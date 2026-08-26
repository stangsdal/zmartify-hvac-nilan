#include "nilan_cts602.h"

#include <string.h>

#include "comm_rs485.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "nilan_adapter";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static nilan_state_t s_state;
static nilan_poll_stats_t s_stats;
static uint8_t s_slave;
static bool s_running;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool decode_words(const uint8_t *payload, size_t payload_length,
                         uint16_t *words, size_t word_count)
{
    if (!payload || !words || payload_length != word_count * 2) return false;
    for (size_t i = 0; i < word_count; ++i) {
        words[i] = ((uint16_t)payload[i * 2] << 8) | payload[i * 2 + 1];
    }
    return true;
}

static bool read_group(uint16_t offset, uint16_t quantity, uint16_t *words)
{
    uint8_t request[8];
    const size_t request_length = nilan_build_read_request(
        s_slave, 4, offset, quantity, request, sizeof(request));
    if (request_length == 0) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.request_count++;
    xSemaphoreGive(s_lock);
    esp_err_t err = comm_rs485_send_frame(request, request_length, 250);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stats.timeout_count++;
        xSemaphoreGive(s_lock);
        return false;
    }

    rs485_frame_t response;
    err = comm_rs485_receive_frame_ex(&response, 600, false);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stats.timeout_count++;
        xSemaphoreGive(s_lock);
        return false;
    }

    uint8_t payload[NILAN_MODBUS_FRAME_MAX];
    size_t payload_length = 0;
    if (!nilan_validate_read_response(response.data, response.len, s_slave, 4,
                                      payload, sizeof(payload), &payload_length) ||
        !decode_words(payload, payload_length, words, quantity)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stats.crc_error_count++;
        xSemaphoreGive(s_lock);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.response_count++;
    xSemaphoreGive(s_lock);
    return true;
}

static bool poll_once(void)
{
    uint16_t control[4], ventilation[5], temperatures[7], alarms[10];
    if (!read_group(NILAN_INPUT_CONTROL_BASE, 4, control)) return false;
    if (!read_group(NILAN_INPUT_VENTILATION_BASE, 5, ventilation)) return false;
    if (!read_group(NILAN_INPUT_TEMPERATURE_BASE, 7, temperatures)) return false;
    if (!read_group(NILAN_INPUT_ALARM_BASE, 10, alarms)) return false;

    nilan_state_t next = {0};
    if (!nilan_decode_state(control, 4, ventilation, 5, temperatures, 7, &next)) {
        return false;
    }
    next.last_success_ms = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = next;
    s_stats.controller_online = true;
    s_stats.last_success_ms = next.last_success_ms;
    xSemaphoreGive(s_lock);
    return true;
}

static void poll_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (s_running) {
        const bool ok = poll_once();
        if (!ok) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_stats.controller_online = false;
            s_state.status = NILAN_VALUE_STALE;
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "CTS602 read-only poll failed; state marked stale");
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(2000));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

bool nilan_adapter_start(uint8_t slave_address)
{
    if (slave_address == 0 || slave_address > 247 || s_running) return false;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_stats, 0, sizeof(s_stats));
    s_state.status = NILAN_VALUE_UNAVAILABLE;
    s_slave = slave_address;
    s_running = true;
    if (xTaskCreate(poll_task, "nilan_poll", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return false;
    }
    return true;
}

bool nilan_adapter_get_state(nilan_state_t *out_state, nilan_poll_stats_t *out_stats)
{
    if (!s_lock || (!out_state && !out_stats)) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_state) *out_state = s_state;
    if (out_stats) *out_stats = s_stats;
    xSemaphoreGive(s_lock);
    return true;
}
