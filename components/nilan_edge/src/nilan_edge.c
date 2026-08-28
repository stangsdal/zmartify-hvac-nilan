#include "nilan_edge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "nilan_cts602.h"
#include "nilan_onboarding.h"
#include "nvs.h"
#include "ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nilan_edge";
static esp_mqtt_client_handle_t s_mqtt;
static httpd_handle_t s_http;
static char s_device_id[64];
static uint32_t s_last_ota_poll_ms;

static bool mqtt_reload_from_nvs(void);
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);

static bool mqtt_json_i32(const char *payload, const char *key, int32_t *out)
{
    char needle[64];
    if (payload == NULL || key == NULL || out == NULL) return false;
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(payload, needle);
    if (p == NULL) return false;
    const char *value = p + strlen(needle);
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value) return false;
    *out = (int32_t)parsed;
    return true;
}

static bool mqtt_json_string(const char *payload, const char *key, char *out, size_t out_len)
{
    char needle[64];
    if (payload == NULL || key == NULL || out == NULL || out_len == 0U) return false;
    (void)snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(payload, needle);
    if (p == NULL) return false;
    p += strlen(needle);
    size_t i = 0U;
    while (p[i] != '\0' && p[i] != '"' && i + 1U < out_len) {
        out[i] = p[i];
        ++i;
    }
    out[i] = '\0';
    return i > 0U;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int append_words(char *out, size_t out_len, int pos, const char *name,
                        const uint16_t *words, size_t count)
{
    if (pos < 0 || (size_t)pos >= out_len) return pos;
    pos += snprintf(out + pos, out_len - (size_t)pos, "\"%s\":[", name);
    for (size_t i = 0; i < count && (size_t)pos < out_len; ++i) {
        pos += snprintf(out + pos, out_len - (size_t)pos, "%s%u", i ? "," : "", words[i]);
    }
    if ((size_t)pos < out_len) pos += snprintf(out + pos, out_len - (size_t)pos, "]");
    return pos;
}

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
    char room_temperature[24];
    char co2[24];
    (void)snprintf(room_temperature, sizeof(room_temperature), state.room_temperature_available ? "%.2f" : "null",
                   state.room_temperature_centi_c / 100.0);
    (void)snprintf(co2, sizeof(co2), state.co2_available ? "%u" : "null", state.co2_ppm);
    return snprintf(out, out_len,
                    "{\"device_id\":\"%s\",\"online\":%s,\"controller_online\":%s,"
                    "\"freshness_age_ms\":%" PRIu32 ",\"run\":%s,\"ventilation_level\":%u,"
                    "\"actual_inlet_level\":%u,\"actual_exhaust_level\":%u,"
                    "\"room_temperature_c\":%s,\"inlet_temperature_c\":%.2f,"
                    "\"outlet_temperature_c\":%.2f,\"extract_temperature_c\":%.2f,"
                    "\"humidity_pct\":%.2f,\"co2_ppm\":%s,\"filter_days_remaining\":%u,"
                    "\"status\":\"%s\",\"poll_requests\":%" PRIu32 ",\"poll_responses\":%" PRIu32 "}",
                    s_device_id, stats.controller_online ? "true" : "false",
                    stats.controller_online ? "true" : "false", age, state.run ? "true" : "false",
                    state.ventilation_level, state.actual_inlet_level, state.actual_exhaust_level,
                    room_temperature,
                    state.inlet_temperature_centi_c / 100.0,
                    state.outlet_temperature_centi_c / 100.0, state.extract_temperature_centi_c / 100.0,
                    state.humidity_centi_pct / 100.0,
                    co2, state.filter_days_remaining,
                    state.status == NILAN_VALUE_FRESH ? "fresh" :
                    (state.status == NILAN_VALUE_STALE ? "stale" : "unavailable"),
                    stats.request_count, stats.response_count);
}

static esp_err_t raw_get_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_DIAGNOSTICS
    nilan_state_t state = {0};
    nilan_poll_stats_t stats = {0};
    if (!nilan_adapter_get_state(&state, &stats)) return ESP_FAIL;
    char body[768];
    int pos = snprintf(body, sizeof(body), "{\"status\":\"%s\",\"control\":[%u,%u,%u,%u],",
                       state.status == NILAN_VALUE_FRESH ? "fresh" :
                       (state.status == NILAN_VALUE_STALE ? "stale" : "unavailable"),
                       state.raw[0], state.raw[1], state.raw[2], state.raw[3]);
    pos = append_words(body, sizeof(body), pos, "ventilation", state.raw_ventilation, 5);
    if ((size_t)pos < sizeof(body)) body[pos++] = ',';
    pos = append_words(body, sizeof(body), pos, "temperature", state.raw_temperature, 7);
    if ((size_t)pos < sizeof(body)) body[pos++] = ',';
    pos = append_words(body, sizeof(body), pos, "alarms", state.raw_alarms, 10);
    if ((size_t)pos < sizeof(body)) {
        pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                        ",\"poll_requests\":%" PRIu32 ",\"poll_responses\":%" PRIu32 "}",
                        stats.request_count, stats.response_count);
    }
    if (pos < 0 || (size_t)pos >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, pos);
#else
    return httpd_resp_send_404(req);
#endif
}

static esp_err_t ventilation_post_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_WRITES
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    char body[64] = {0};
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;
    char *level_text = strstr(body, "level");
    if (!level_text) return httpd_resp_send_404(req);
    level_text = strchr(level_text, ':');
    if (!level_text) level_text = strchr(body, '=');
    if (!level_text) return httpd_resp_send_404(req);
    const long level = strtol(level_text + 1, NULL, 10);
    uint16_t readback = 0;
    const bool ok = nilan_adapter_set_ventilation((uint8_t)level, &readback);
    char response[192];
    (void)snprintf(response, sizeof(response),
                   "{\"ok\":%s,\"requested_level\":%ld,\"readback_level\":%u}",
                   ok ? "true" : "false", level, readback);
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send_404(req);
#endif
}

static esp_err_t inlet_speed_post_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_WRITES
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    char body[64] = {0};
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;
    char *value_text = strstr(body, "inlet_pct");
    if (!value_text) return httpd_resp_send_404(req);
    value_text = strchr(value_text, ':');
    if (!value_text) value_text = strchr(body, '=');
    if (!value_text) return httpd_resp_send_404(req);
    const long pct = strtol(value_text + 1, NULL, 10);
    uint16_t readback = 0;
    const bool ok = pct >= 0 && pct <= 100 &&
                    nilan_adapter_set_inlet_speed_pct((uint16_t)pct, &readback);
    char response[192];
    (void)snprintf(response, sizeof(response),
                   "{\"ok\":%s,\"requested_inlet_pct\":%ld,\"readback_inlet_pct\":%u}",
                   ok ? "true" : "false", pct, readback / 100U);
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send_404(req);
#endif
}

static esp_err_t exhaust_speed_post_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_WRITES
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    char body[64] = {0};
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;
    char *value_text = strstr(body, "exhaust_pct");
    if (!value_text) return httpd_resp_send_404(req);
    value_text = strchr(value_text, ':');
    if (!value_text) value_text = strchr(body, '=');
    if (!value_text) return httpd_resp_send_404(req);
    const long pct = strtol(value_text + 1, NULL, 10);
    uint16_t readback = 0;
    const bool ok = pct >= 0 && pct <= 100 &&
                    nilan_adapter_set_exhaust_speed_pct((uint16_t)pct, &readback);
    char response[192];
    (void)snprintf(response, sizeof(response),
                   "{\"ok\":%s,\"requested_exhaust_pct\":%ld,\"readback_exhaust_pct\":%u}",
                   ok ? "true" : "false", pct, readback / 100U);
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send_404(req);
#endif
}

static int status_json(char *out, size_t out_len)
{
    nilan_state_t state = {0};
    nilan_poll_stats_t stats = {0};
    ota_status_t ota = {0};
    if (!nilan_adapter_get_state(&state, &stats)) return -1;
    (void)ota_get_status(&ota);
    const uint32_t age = state.last_success_ms > 0 ? now_ms() - state.last_success_ms : 0;
    return snprintf(out, out_len,
                    "{\"api_started\":true,\"read_only_adapter_started\":true,"
                    "\"controller_online\":%s,\"state_status\":\"%s\","
                    "\"uptime_ms\":%" PRIu32 ",\"free_heap_bytes\":%" PRIu32 ","
                    "\"min_free_heap_bytes\":%" PRIu32 ",\"freshness_age_ms\":%" PRIu32 ","
                    "\"poll_requests\":%" PRIu32 ",\"poll_responses\":%" PRIu32 ","
                    "\"timeout_count\":%" PRIu32 ",\"crc_error_count\":%" PRIu32 ","
                    "\"framing_error_count\":%" PRIu32 ",\"exception_count\":%" PRIu32 ","
                    "\"retry_count\":%" PRIu32 ",\"last_success_ms\":%" PRIu32 ","
                    "\"write_success_count\":%" PRIu32 ",\"write_error_count\":%" PRIu32 ","
                    "\"last_write_requested\":%u,\"last_write_readback\":%u,"
                    "\"last_write_ok\":%s,\"last_write_ms\":%" PRIu32 ","
                    "\"ota\":{\"service_started\":%s,\"pending_verify\":%s,"
                    "\"last_update_ok\":%s,\"last_update_bytes\":%" PRIu32 ","
                    "\"last_error\":%d}}",
                    stats.controller_online ? "true" : "false",
                    state.status == NILAN_VALUE_FRESH ? "fresh" :
                    (state.status == NILAN_VALUE_STALE ? "stale" : "unavailable"),
                    now_ms(), (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    (uint32_t)esp_get_minimum_free_heap_size(), age, stats.request_count,
                    stats.response_count, stats.timeout_count, stats.crc_error_count,
                    stats.framing_error_count, stats.exception_count, stats.retry_count,
                    stats.last_success_ms, stats.write_success_count, stats.write_error_count,
                    stats.last_write_requested, stats.last_write_readback,
                    stats.last_write_ok ? "true" : "false", stats.last_write_ms,
                    ota.service_started ? "true" : "false",
                    ota.pending_verify ? "true" : "false",
                    ota.last_update_ok ? "true" : "false", ota.last_update_bytes,
                    (int)ota.last_error);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    nilan_poll_stats_t stats = {0};
    ota_status_t ota = {0};
    if (!nilan_adapter_get_state(NULL, &stats)) return ESP_FAIL;
    (void)ota_get_status(&ota);
    const bool controller_online = stats.controller_online;
    char body[512];
    const int len = snprintf(body, sizeof(body),
                             "{\"ok\":%s,\"degraded\":%s,"
                             "\"checks\":{\"http_server_started\":true,"
                             "\"read_only_adapter_started\":true,"
                             "\"controller_online\":%s},\"ota\":{\"service_started\":%s,"
                             "\"pending_verify\":%s},\"poll_requests\":%" PRIu32 ","
                             "\"poll_responses\":%" PRIu32 "}",
                             controller_online ? "true" : "false",
                             controller_online ? "false" : "true",
                             controller_online ? "true" : "false",
                             ota.service_started ? "true" : "false",
                             ota.pending_verify ? "true" : "false", stats.request_count,
                             stats.response_count);
    if (len < 0 || (size_t)len >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[1024];
    const int len = status_json(body, sizeof(body));
    if (len < 0 || (size_t)len >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static void sha256_to_hex(const uint8_t *hash, size_t hash_len, char *out, size_t out_len)
{
    if (!hash || !out || out_len < (hash_len * 2U) + 1U) return;
    for (size_t i = 0; i < hash_len; ++i) {
        (void)snprintf(&out[i * 2U], out_len - (i * 2U), "%02x", hash[i]);
    }
}

static esp_err_t version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char hash_hex[(sizeof(desc->app_elf_sha256) * 2U) + 1U];
    sha256_to_hex((const uint8_t *)desc->app_elf_sha256, sizeof(desc->app_elf_sha256),
                  hash_hex, sizeof(hash_hex));
    char body[384];
    const int len = snprintf(body, sizeof(body),
                             "{\"project\":\"%s\",\"version\":\"%s\","
                             "\"idf\":\"%s\",\"build_time\":\"%s %s\","
                             "\"app_elf_sha256\":\"%s\",\"hardware\":\"esp32-s3r8\","
                             "\"app_version\":\"%s\",\"bus_version\":\"%s\"}",
                             desc->project_name, desc->version, esp_get_idf_version(),
                             desc->date, desc->time, hash_hex, NILAN_FIRMWARE_VERSION,
                             NILAN_BUS_VERSION);
    if (len < 0 || (size_t)len >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_OTA
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    uint32_t written = 0;
    const esp_err_t err = ota_handle_http_upload(req, &written);
    if (err != ESP_OK) {
        char body[160];
        (void)snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                       esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    char body[192];
    (void)snprintf(body, sizeof(body),
                   "{\"ok\":true,\"written_bytes\":%" PRIu32 ",\"reboot_required\":true}",
                   written);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send_404(req);
#endif
}

static void reboot_timer_callback(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
#if CONFIG_NILAN_ENABLE_DEV_OTA
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    if (err != ESP_OK) return err;
    const esp_timer_create_args_t timer_args = {
        .callback = reboot_timer_callback, .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK, .name = "nilan_reboot",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&timer_args, &timer) != ESP_OK) return ESP_ERR_NO_MEM;
    if (esp_timer_start_once(timer, 150000) != ESP_OK) {
        (void)esp_timer_delete(timer);
        return ESP_FAIL;
    }
    return ESP_OK;
#else
    return httpd_resp_send_404(req);
#endif
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

static void mqtt_publish_homie_discovery(void)
{
    if (!s_mqtt) return;

    char topic[192];
    (void)snprintf(topic, sizeof(topic), "homie/5/%s/$state", s_device_id);
    (void)esp_mqtt_client_publish(s_mqtt, topic, "init", 4, 1, 1);

    (void)snprintf(topic, sizeof(topic), "homie/5/%s/$description", s_device_id);
    const char *description =
        "{\"homie\":\"5.0\",\"name\":\"Nilan Comfort 302\","
        "\"nodes\":[{\"id\":\"nilan\",\"name\":\"Nilan CTS602\","
        "\"type\":\"hvac\",\"properties\":["
        "{\"id\":\"state\",\"name\":\"State\",\"datatype\":\"string\"},"
        "{\"id\":\"ventilation-level\",\"name\":\"Ventilation level\",\"datatype\":\"integer\",\"format\":\"0..4\"},"
        "{\"id\":\"inlet-temperature\",\"name\":\"Inlet temperature\",\"datatype\":\"float\",\"unit\":\"C\"},"
        "{\"id\":\"outlet-temperature\",\"name\":\"Outlet temperature\",\"datatype\":\"float\",\"unit\":\"C\"},"
        "{\"id\":\"humidity\",\"name\":\"Humidity\",\"datatype\":\"float\",\"unit\":\"%\"}]}]}";
    (void)esp_mqtt_client_publish(s_mqtt, topic, description, 0, 1, 1);

    (void)snprintf(topic, sizeof(topic), "homie/5/%s/$state", s_device_id);
    (void)esp_mqtt_client_publish(s_mqtt, topic, "ready", 5, 1, 1);
}

static void mqtt_publish_command_outcome(const char *command_id, const char *command,
                                         bool ok, const char *detail)
{
    if (!s_mqtt) return;
    char topic[192], payload[512];
    (void)snprintf(topic, sizeof(topic), "zmartify/v2/devices/%s/events/hvac/command-outcome", s_device_id);
    (void)snprintf(payload, sizeof(payload),
                   "{\"schema_version\":\"2.0\",\"command_id\":\"%.64s\","
                   "\"command_type\":\"%.64s\",\"result\":\"%s\",\"detail\":\"%.160s\"}",
                   command_id != NULL ? command_id : "device-generated", command != NULL ? command : "unknown",
                   ok ? "confirmed" : "failed", detail != NULL ? detail : "");
    (void)esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
}

static bool json_true(const char *payload, const char *key)
{
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "\"%s\":true", key);
    return payload != NULL && strstr(payload, needle) != NULL;
}

static void ota_pull_task(void *arg)
{
    (void)arg;
    char edge[160] = {0}, token[128] = {0};
    nvs_handle_t nvs = 0;
    if (nvs_open("cfg", NVS_READONLY, &nvs) != ESP_OK) goto done;
    size_t len = sizeof(edge); (void)nvs_get_str(nvs, "edge_url", edge, &len);
    len = sizeof(token); (void)nvs_get_str(nvs, "ob_admin_token", token, &len);
    nvs_close(nvs);
    if (edge[0] == '\0' || token[0] == '\0') goto done;
    const esp_app_desc_t *app = esp_app_get_description();
    char url[360];
    (void)snprintf(url, sizeof(url), "%s%sapi/v2/devices/%s/ota/poll?current_version=%s",
                   edge, edge[strlen(edge) - 1U] == '/' ? "" : "/", s_device_id,
                   app != NULL ? app->version : "unknown");
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_GET, .timeout_ms = 15000,
                                    .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto done;
    char auth[192]; (void)snprintf(auth, sizeof(auth), "Bearer %s", token);
    (void)esp_http_client_set_header(client, "Authorization", auth);
    if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) < 200 || esp_http_client_get_status_code(client) >= 300) {
        esp_http_client_close(client); esp_http_client_cleanup(client); goto done;
    }
    char response[1200] = {0};
    const int n = esp_http_client_read(client, response, sizeof(response) - 1U);
    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (n <= 0 || !json_true(response, "update_available")) goto done;
    char download[360] = {0}, sha256[65] = {0};
    if (!mqtt_json_string(response, "download_url", download, sizeof(download)) ||
        !mqtt_json_string(response, "sha256", sha256, sizeof(sha256))) goto done;
    uint32_t written = 0U;
    if (ota_pull_and_apply(download, token, sha256, &written) == ESP_OK) {
        ESP_LOGI(TAG, "Edge-staged OTA applied: %" PRIu32 " bytes", written);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
done:
    vTaskDelete(NULL);
}

static void ota_poll_start(void)
{
    if (xTaskCreate(ota_pull_task, "nilan_ota_pull", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Could not start Edge OTA poll task");
    }
}

static void mqtt_handle_nilan_command(const char *topic, const char *payload)
{
#if CONFIG_NILAN_ENABLE_MQTT_COMMANDS
    if (strstr(topic, "/gateway/ota-check/set") != NULL) {
        if (payload != NULL && (strstr(payload, "1") != NULL || json_true(payload, "check"))) ota_poll_start();
        return;
    }
    char command_id[72] = {0};
    (void)mqtt_json_string(payload, "command_id", command_id, sizeof(command_id));
    int32_t value = -1;
    bool ok = false;
    const char *command = "unknown";
    char detail[160] = "invalid command payload";
    if (strstr(topic, "/commands/hvac/ventilation") != NULL) {
        command = "hvac.set_ventilation_level";
        if (mqtt_json_i32(payload, "level", &value)) {
            uint16_t readback = 0;
            ok = value >= 0 && value <= 4 && nilan_adapter_set_ventilation((uint8_t)value, &readback);
            (void)snprintf(detail, sizeof(detail), "requested=%ld readback=%u", (long)value, readback);
        }
    } else if (strstr(topic, "/commands/hvac/inlet-speed") != NULL) {
        command = "hvac.set_inlet_speed";
        if (mqtt_json_i32(payload, "inlet_pct", &value)) {
            uint16_t readback = 0;
            ok = value >= 0 && value <= 100 && nilan_adapter_set_inlet_speed_pct((uint16_t)value, &readback);
            (void)snprintf(detail, sizeof(detail), "requested=%ld readback=%u", (long)value, readback / 100U);
        }
    } else if (strstr(topic, "/commands/hvac/exhaust-speed") != NULL) {
        command = "hvac.set_exhaust_speed";
        if (mqtt_json_i32(payload, "exhaust_pct", &value)) {
            uint16_t readback = 0;
            ok = value >= 0 && value <= 100 && nilan_adapter_set_exhaust_speed_pct((uint16_t)value, &readback);
            (void)snprintf(detail, sizeof(detail), "requested=%ld readback=%u", (long)value, readback / 100U);
        }
    } else {
        return;
    }
    mqtt_publish_command_outcome(command_id, command, ok, detail);
#else
    (void)topic; (void)payload;
#endif
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    if (event_id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_event_handle_t event = event_data;
        s_mqtt = event->client;
        mqtt_publish_homie_discovery();
        mqtt_publish_state();
#if CONFIG_NILAN_ENABLE_MQTT_COMMANDS
        char topic[192];
        (void)snprintf(topic, sizeof(topic), "zmartify/v2/devices/%s/commands/hvac/ventilation", s_device_id);
        (void)esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        (void)snprintf(topic, sizeof(topic), "zmartify/v2/devices/%s/commands/hvac/inlet-speed", s_device_id);
        (void)esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        (void)snprintf(topic, sizeof(topic), "zmartify/v2/devices/%s/commands/hvac/exhaust-speed", s_device_id);
        (void)esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        (void)snprintf(topic, sizeof(topic), "homie/5/%s/gateway/ota-check/set", s_device_id);
        (void)esp_mqtt_client_subscribe(s_mqtt, topic, 1);
#endif
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt = NULL;
    } else if (event_id == MQTT_EVENT_DATA) {
        esp_mqtt_event_handle_t event = event_data;
        if (event == NULL || event->topic == NULL || event->data == NULL) return;
        char topic[256], payload[512];
        const size_t topic_len = event->topic_len < sizeof(topic) - 1U ? (size_t)event->topic_len : sizeof(topic) - 1U;
        const size_t data_len = event->data_len < sizeof(payload) - 1U ? (size_t)event->data_len : sizeof(payload) - 1U;
        memcpy(topic, event->topic, topic_len); topic[topic_len] = '\0';
        memcpy(payload, event->data, data_len); payload[data_len] = '\0';
        mqtt_handle_nilan_command(topic, payload);
    }
}

static void publisher_task(void *arg)
{
    (void)arg;
    while (true) {
        mqtt_publish_state();
        if (s_mqtt != NULL && now_ms() - s_last_ota_poll_ms >= 300000U) {
            s_last_ota_poll_ms = now_ms();
            ota_poll_start();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static bool mqtt_reload_from_nvs(void)
{
    if (s_mqtt != NULL) {
        (void)esp_mqtt_client_stop(s_mqtt);
        (void)esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }

    char uri[128] = {0}, user[64] = {0}, password[64] = {0};
    load_mqtt_config(uri, sizeof(uri), user, sizeof(user), password, sizeof(password));
    if (uri[0] == '\0') return true;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s_device_id,
    };
    if (user[0]) cfg.credentials.username = user;
    if (password[0]) cfg.credentials.authentication.password = password;
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (s_mqtt == NULL ||
        esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK ||
        esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        s_mqtt = NULL;
        return false;
    }
    return true;
}

bool nilan_edge_start(void)
{
    nilan_onboarding_init(mqtt_reload_from_nvs);
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)snprintf(s_device_id, sizeof(s_device_id), "zmartify-hvac-nilan-%02x%02x%02x",
                   mac[3], mac[4], mac[5]);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 20;
    if (httpd_start(&s_http, &http_cfg) != ESP_OK) return false;
    const httpd_uri_t state_uri = {
        .uri = "/api/v1/nilan/state", .method = HTTP_GET, .handler = state_get_handler
    };
    if (httpd_register_uri_handler(s_http, &state_uri) != ESP_OK) return false;
    const httpd_uri_t health_uri = {
        .uri = "/health", .method = HTTP_GET, .handler = health_get_handler
    };
    const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler
    };
    const httpd_uri_t version_uri = {
        .uri = "/version", .method = HTTP_GET, .handler = version_get_handler
    };
    const httpd_uri_t raw_uri = {
        .uri = "/api/v1/nilan/raw", .method = HTTP_GET, .handler = raw_get_handler
    };
    const httpd_uri_t ventilation_uri = {
        .uri = "/api/v1/nilan/ventilation", .method = HTTP_POST,
        .handler = ventilation_post_handler
    };
    const httpd_uri_t inlet_speed_uri = {
        .uri = "/api/v1/nilan/inlet-speed", .method = HTTP_POST,
        .handler = inlet_speed_post_handler
    };
    const httpd_uri_t exhaust_speed_uri = {
        .uri = "/api/v1/nilan/exhaust-speed", .method = HTTP_POST,
        .handler = exhaust_speed_post_handler
    };
    const httpd_uri_t identity_uri = {
        .uri = "/identity", .method = HTTP_GET, .handler = nilan_identity_get_handler
    };
    const httpd_uri_t claim_token_uri = {
        .uri = "/claim-token", .method = HTTP_GET, .handler = nilan_claim_token_get_handler
    };
    const httpd_uri_t onboarding_status_uri = {
        .uri = "/onboarding/status", .method = HTTP_GET,
        .handler = nilan_onboarding_status_get_handler
    };
    const httpd_uri_t onboarding_configure_uri = {
        .uri = "/onboarding/configure", .method = HTTP_POST,
        .handler = nilan_onboarding_configure_post_handler
    };
    const httpd_uri_t onboarding_reset_uri = {
        .uri = "/onboarding/reset", .method = HTTP_POST,
        .handler = nilan_onboarding_reset_post_handler
    };
    if (httpd_register_uri_handler(s_http, &health_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &status_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &version_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &identity_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &claim_token_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &onboarding_status_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &onboarding_configure_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &onboarding_reset_uri) != ESP_OK) return false;
#if CONFIG_NILAN_ENABLE_DEV_DIAGNOSTICS
    if (httpd_register_uri_handler(s_http, &raw_uri) != ESP_OK) return false;
#endif
#if CONFIG_NILAN_ENABLE_DEV_WRITES
    if (httpd_register_uri_handler(s_http, &ventilation_uri) != ESP_OK) return false;
    if (httpd_register_uri_handler(s_http, &inlet_speed_uri) != ESP_OK) return false;
    if (httpd_register_uri_handler(s_http, &exhaust_speed_uri) != ESP_OK) return false;
#endif
#if CONFIG_NILAN_ENABLE_DEV_OTA
    const httpd_uri_t ota_uri = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler
    };
    const httpd_uri_t reboot_uri = {
        .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler
    };
    if (httpd_register_uri_handler(s_http, &ota_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http, &reboot_uri) != ESP_OK) return false;
#endif

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
