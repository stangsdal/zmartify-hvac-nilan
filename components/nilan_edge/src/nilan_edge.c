#include "nilan_edge.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "nilan_cts602.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nilan_edge";
static esp_mqtt_client_handle_t s_mqtt;
static httpd_handle_t s_http;
static char s_device_id[64];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void load_mqtt_config(char *uri, size_t uri_len, char *user, size_t user_len,
                             char *password, size_t password_len)
{
    nvs_handle_t nvs;
    if (nvs_open("cfg", NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t enabled = 0;
    (void)nvs_get_u8(nvs, "mqtt_enabled", &enabled);
    (void)nvs_get_str(nvs, "mqtt_uri", uri, &uri_len);
    (void)nvs_get_str(nvs, "mqtt_username", user, &user_len);
    (void)nvs_get_str(nvs, "mqtt_password", password, &password_len);
    nvs_close(nvs);
    if (enabled == 0) uri[0] = '\0';
}

static int state_json(char *out, size_t out_len)
{
    nilan_state_t state = {0};
    nilan_poll_stats_t stats = {0};
    if (!nilan_adapter_get_state(&state, &stats)) return -1;
    const uint32_t age = state.last_success_ms > 0 ? now_ms() - state.last_success_ms : 0;
    return snprintf(out, out_len,
                    "{\"device_id\":\"%s\",\"online\":%s,\"controller_online\":%s,"
                    "\"freshness_age_ms\":%" PRIu32 ",\"run\":%s,\"ventilation_level\":%u,"
                    "\"actual_inlet_level\":%u,\"actual_exhaust_level\":%u,"
                    "\"room_temperature_c\":%.2f,\"inlet_temperature_c\":%.2f,"
                    "\"outlet_temperature_c\":%.2f,\"extract_temperature_c\":%.2f,"
                    "\"humidity_pct\":%.2f,\"co2_ppm\":%u,\"filter_days_remaining\":%u,"
                    "\"status\":\"%s\",\"poll_requests\":%" PRIu32 ",\"poll_responses\":%" PRIu32 "}",
                    s_device_id, stats.controller_online ? "true" : "false",
                    stats.controller_online ? "true" : "false", age, state.run ? "true" : "false",
                    state.ventilation_level, state.actual_inlet_level, state.actual_exhaust_level,
                    state.room_temperature_centi_c / 100.0, state.inlet_temperature_centi_c / 100.0,
                    state.outlet_temperature_centi_c / 100.0, state.extract_temperature_centi_c / 100.0,
                    state.humidity_centi_pct / 100.0, state.co2_ppm, state.filter_days_remaining,
                    state.status == NILAN_VALUE_FRESH ? "fresh" :
                    (state.status == NILAN_VALUE_STALE ? "stale" : "unavailable"),
                    stats.request_count, stats.response_count);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    char body[1600];
    const int len = state_json(body, sizeof(body));
    if (len < 0 || (size_t)len >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static void mqtt_publish_state(void)
{
    if (!s_mqtt) return;
    char payload[1600], topic[192];
    const int len = state_json(payload, sizeof(payload));
    if (len < 0 || (size_t)len >= sizeof(payload)) return;
    (void)snprintf(topic, sizeof(topic), "homie/5/%s/nilan/state", s_device_id);
    (void)esp_mqtt_client_publish(s_mqtt, topic, payload, len, 1, 1);
    (void)snprintf(topic, sizeof(topic), "zmartify/v2/devices/%s/state/hvac", s_device_id);
    (void)esp_mqtt_client_publish(s_mqtt, topic, payload, len, 1, 1);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    if (event_id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_event_handle_t event = event_data;
        s_mqtt = event->client;
        mqtt_publish_state();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt = NULL;
    }
}

static void publisher_task(void *arg)
{
    (void)arg;
    while (true) {
        mqtt_publish_state();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

bool nilan_edge_start(void)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)snprintf(s_device_id, sizeof(s_device_id), "zmartify-hvac-nilan-%02x%02x%02x",
                   mac[3], mac[4], mac[5]);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_http, &http_cfg) != ESP_OK) return false;
    const httpd_uri_t state_uri = {
        .uri = "/api/v1/nilan/state", .method = HTTP_GET, .handler = state_get_handler
    };
    if (httpd_register_uri_handler(s_http, &state_uri) != ESP_OK) return false;

    char uri[128] = {0}, user[64] = {0}, password[64] = {0};
    load_mqtt_config(uri, sizeof(uri), user, sizeof(user), password, sizeof(password));
    if (uri[0] != '\0') {
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = uri,
            .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
            .credentials.client_id = s_device_id,
        };
        if (user[0]) cfg.credentials.username = user;
        if (password[0]) cfg.credentials.authentication.password = password;
        s_mqtt = esp_mqtt_client_init(&cfg);
        if (!s_mqtt ||
            esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK ||
            esp_mqtt_client_start(s_mqtt) != ESP_OK) return false;
    } else {
        ESP_LOGI(TAG, "MQTT disabled or not configured");
    }
    return xTaskCreate(publisher_task, "nilan_pub", 4096, NULL, 4, NULL) == pdPASS;
}
