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
static SemaphoreHandle_t s_queue_lock;
static TaskHandle_t s_task;
static nilan_state_t s_state;
static nilan_poll_stats_t s_stats;
static uint8_t s_slave;
static bool s_running;
static uint8_t s_consecutive_poll_failures;

typedef struct {
    uint16_t offset;
    uint16_t value;
    uint16_t readback;
    bool ok;
    SemaphoreHandle_t completed;
} nilan_write_request_t;

#define NILAN_WRITE_QUEUE_DEPTH 4
static nilan_write_request_t *s_write_queue[NILAN_WRITE_QUEUE_DEPTH];
static uint8_t s_write_queue_count;

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

static bool read_group_function(uint8_t function, uint16_t offset, uint16_t quantity, uint16_t *words)
{
    uint8_t request[8];
    const size_t request_length = nilan_build_read_request(
        s_slave, function, offset, quantity, request, sizeof(request));
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
    if (!nilan_validate_read_response(response.data, response.len, s_slave, function,
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

static bool read_group(uint16_t offset, uint16_t quantity, uint16_t *words)
{
    return read_group_function(4, offset, quantity, words);
}

static void process_writes(void);

static bool poll_once(void)
{
    uint16_t control[4], ventilation[5], temperatures[7], alarms[10], control_sets[6];
    uint16_t inlet_speed_raw = 0, exhaust_speed_raw = 0;
    if (!read_group(NILAN_INPUT_CONTROL_BASE, 4, control)) return false;
    process_writes();
    if (!read_group(NILAN_INPUT_VENTILATION_BASE, 5, ventilation)) return false;
    process_writes();
    if (!read_group(NILAN_INPUT_TEMPERATURE_BASE, 7, temperatures)) return false;
    process_writes();
    if (!read_group(NILAN_INPUT_ALARM_BASE, 10, alarms)) return false;
    process_writes();
    if (!read_group_function(3, NILAN_HOLDING_RUN, 6, control_sets)) return false;
    process_writes();
    if (!read_group_function(3, NILAN_HOLDING_INLET_SPEED, 1, &inlet_speed_raw)) return false;
    process_writes();
    if (!read_group_function(3, NILAN_HOLDING_EXHAUST_SPEED, 1, &exhaust_speed_raw)) return false;

    nilan_state_t next = {0};
    if (!nilan_decode_state(control, 4, ventilation, 5, temperatures, 7, &next)) {
        return false;
    }
    memcpy(next.raw_alarms, alarms, sizeof(next.raw_alarms));
    next.inlet_speed = inlet_speed_raw > 10000 ? 100 : inlet_speed_raw / 100;
    next.exhaust_speed = exhaust_speed_raw > 10000 ? 100 : exhaust_speed_raw / 100;
    next.run_set = control_sets[0];
    next.mode_set = control_sets[1];
    next.vent_set = control_sets[2];
    next.temp_set = control_sets[3];
    next.service_mode = control_sets[4];
    next.service_pct = control_sets[5] > 10000 ? 100 : control_sets[5] / 100;
    next.last_success_ms = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = next;
    s_stats.controller_online = true;
    s_stats.last_success_ms = next.last_success_ms;
    s_consecutive_poll_failures = 0;
    xSemaphoreGive(s_lock);
    return true;
}

static bool perform_write(uint16_t offset, uint16_t value, uint16_t *out_readback)
{
    uint8_t request[11];
    const size_t request_length = nilan_build_write_request(
        s_slave, offset, value, request, sizeof(request));
    if (request_length == 0 ||
        comm_rs485_send_frame_with_priority(request, request_length, 250,
                                             RS485_TX_PRIORITY_HIGH) != ESP_OK) {
        return false;
    }

    rs485_frame_t response;
    if (comm_rs485_receive_frame_ex(&response, 600, false) != ESP_OK ||
        !nilan_validate_write_response(response.data, response.len, s_slave,
                                       offset, 1)) {
        return false;
    }

    uint16_t readback = 0;
    const bool read_ok = read_group_function(3, offset, 1, &readback);
    if (out_readback) *out_readback = readback;
    return read_ok && readback == value;
}

static bool dequeue_write(nilan_write_request_t **out_request)
{
    if (!out_request || !s_queue_lock) return false;
    xSemaphoreTake(s_queue_lock, portMAX_DELAY);
    if (s_write_queue_count == 0) {
        xSemaphoreGive(s_queue_lock);
        return false;
    }
    *out_request = s_write_queue[0];
    for (uint8_t i = 1; i < s_write_queue_count; ++i) {
        s_write_queue[i - 1] = s_write_queue[i];
    }
    s_write_queue_count--;
    xSemaphoreGive(s_queue_lock);
    return true;
}

static void process_writes(void)
{
    nilan_write_request_t *request = NULL;
    while (dequeue_write(&request)) {
        request->readback = 0;
        request->ok = perform_write(request->offset, request->value,
                                    &request->readback);
        xSemaphoreGive(request->completed);
    }
}

static bool submit_write(uint16_t offset, uint16_t value, uint16_t *out_readback)
{
    if (!s_running || !s_queue_lock || !s_task) return false;

    nilan_write_request_t request = {
        .offset = offset,
        .value = value,
        .completed = xSemaphoreCreateBinary(),
    };
    if (!request.completed) return false;

    bool queued = false;
    xSemaphoreTake(s_queue_lock, portMAX_DELAY);
    if (s_write_queue_count < NILAN_WRITE_QUEUE_DEPTH) {
        s_write_queue[s_write_queue_count++] = &request;
        queued = true;
    }
    xSemaphoreGive(s_queue_lock);
    if (!queued) {
        vSemaphoreDelete(request.completed);
        return false;
    }

    xTaskNotifyGive(s_task);
    xSemaphoreTake(request.completed, portMAX_DELAY);
    if (out_readback) *out_readback = request.readback;
    vSemaphoreDelete(request.completed);
    return request.ok;
}

static void poll_task(void *arg)
{
    (void)arg;
    while (s_running) {
        process_writes();
        const bool ok = poll_once();
        process_writes();
        if (!ok) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_consecutive_poll_failures < UINT8_MAX) s_consecutive_poll_failures++;
            if (s_consecutive_poll_failures >= 5) {
                s_stats.controller_online = false;
                s_state.status = NILAN_VALUE_STALE;
            }
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "CTS602 read-only poll failed; state marked stale");
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

bool nilan_adapter_start(uint8_t slave_address)
{
    if (slave_address == 0 || slave_address > 247 || s_running) return false;
    s_lock = xSemaphoreCreateMutex();
    s_queue_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_queue_lock) return false;
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_write_queue, 0, sizeof(s_write_queue));
    s_write_queue_count = 0;
    s_state.status = NILAN_VALUE_UNAVAILABLE;
    s_slave = slave_address;
    s_running = true;
    if (xTaskCreate(poll_task, "nilan_poll", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        vSemaphoreDelete(s_queue_lock);
        s_queue_lock = NULL;
        return false;
    }
    return true;
}

bool nilan_adapter_set_ventilation(uint8_t level, uint16_t *out_readback)
{
    if (level < 1 || level > 4 || !s_lock || !s_queue_lock) return false;
    nilan_command_t command = {NILAN_CMD_SET_VENTILATION, level};
    if (!nilan_validate_command(&command)) return false;
    uint16_t readback = 0;
    const bool ok = submit_write(NILAN_HOLDING_VENTILATION, level, &readback);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.last_write_requested = level;
    s_stats.last_write_readback = readback;
    s_stats.last_write_ok = ok;
    s_stats.last_write_ms = now_ms();
    if (ok) s_stats.write_success_count++; else s_stats.write_error_count++;
    xSemaphoreGive(s_lock);
    if (out_readback) *out_readback = readback;
    return ok;
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

bool nilan_adapter_set_inlet_speed_pct(uint16_t pct, uint16_t *out_readback)
{
    if (pct > 100 || !s_lock || !s_queue_lock) return false;
    const uint16_t value = (uint16_t)(pct * 100U);
    uint16_t readback = 0;
    const bool ok = submit_write(NILAN_HOLDING_INLET_SPEED, value, &readback);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.last_write_requested = value;
    s_stats.last_write_readback = readback;
    s_stats.last_write_ok = ok;
    s_stats.last_write_ms = now_ms();
    if (ok) s_stats.write_success_count++; else s_stats.write_error_count++;
    xSemaphoreGive(s_lock);
    if (out_readback) *out_readback = readback;
    return ok;
}

bool nilan_adapter_set_exhaust_speed_pct(uint16_t pct, uint16_t *out_readback)
{
    if (pct > 100 || !s_lock || !s_queue_lock) return false;
    const uint16_t value = (uint16_t)(pct * 100U);
    uint16_t readback = 0;
    const bool ok = submit_write(NILAN_HOLDING_EXHAUST_SPEED, value, &readback);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.last_write_requested = value;
    s_stats.last_write_readback = readback;
    s_stats.last_write_ok = ok;
    s_stats.last_write_ms = now_ms();
    if (ok) s_stats.write_success_count++; else s_stats.write_error_count++;
    xSemaphoreGive(s_lock);
    if (out_readback) *out_readback = readback;
    return ok;
}

bool nilan_adapter_set_control_register(uint16_t offset, uint16_t value, uint16_t *out_readback)
{
    if (!s_lock || !s_queue_lock) return false;
    uint16_t readback = 0;
    const bool ok = submit_write(offset, value, &readback);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.last_write_requested = value;
    s_stats.last_write_readback = readback;
    s_stats.last_write_ok = ok;
    s_stats.last_write_ms = now_ms();
    if (ok) s_stats.write_success_count++; else s_stats.write_error_count++;
    xSemaphoreGive(s_lock);
    if (out_readback) *out_readback = readback;
    return ok;
}
