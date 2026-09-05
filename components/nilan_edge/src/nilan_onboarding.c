#include "nilan_onboarding.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define CLAIM_TOKEN_LIFETIME_US (600ULL * 1000000ULL)
#define TOKEN_MAX_LEN 128U
#define BODY_MAX_LEN 4096U

static nilan_onboarding_reload_mqtt_fn s_reload_mqtt;
static char s_claim_token[7];
static uint64_t s_claim_expires_us;
static char s_onboarding_body[BODY_MAX_LEN];

static void mqtt_reload_task(void *arg)
{
    (void)arg;
    const bool reloaded = s_reload_mqtt != NULL && s_reload_mqtt();
    ESP_LOGI("nilan_onboarding", "MQTT reload after onboarding: %s", reloaded ? "ok" : "failed");
    vTaskDelete(NULL);
}

static void device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)snprintf(out, out_len, "zmartify-hvac-nilan-%02x%02x%02x",
                   mac[3], mac[4], mac[5]);
}

static bool nvs_string(const char *key, char *out, size_t out_len)
{
    nvs_handle_t nvs = 0;
    if (out == NULL || out_len == 0U) return false;
    out[0] = '\0';
    if (nvs_open("cfg", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = out_len;
    const esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool onboarding_state(char *out, size_t out_len)
{
    if (!nvs_string("ob_state", out, out_len) || out[0] == '\0') {
        (void)snprintf(out, out_len, "unclaimed");
    }
    return true;
}

static bool json_string(const char *body, const char *key, char *out, size_t out_len)
{
    char needle[64];
    if (body == NULL || key == NULL || out == NULL || out_len == 0U) return false;
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (p == NULL) return false;
    p = strchr(p, ':');
    if (p == NULL) return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != '"') return false;
    size_t i = 0U;
    while (*p != '\0' && *p != '"') {
        if ((unsigned char)*p < 0x20U || i + 1U >= out_len) return false;
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return i > 0U;
}

static bool json_i32(const char *body, const char *key, int32_t *out)
{
    char needle[64];
    if (body == NULL || key == NULL || out == NULL) return false;
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (p == NULL) return false;
    p = strchr(p, ':');
    if (p == NULL) return false;
    char *end = NULL;
    const long value = strtol(p + 1, &end, 10);
    if (end == p + 1) return false;
    *out = (int32_t)value;
    return true;
}

static bool read_body(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len < 2U) return false;
    ESP_LOGI("nilan_onboarding", "request content length=%d, buffer=%u",
             (int)req->content_len, (unsigned)out_len);
    if ((size_t)req->content_len >= out_len) {
        ESP_LOGW("nilan_onboarding", "request body rejected before receive");
        return false;
    }
    size_t total = 0U;
    unsigned timeout_retries = 0U;
    ESP_LOGI("nilan_onboarding", "body length=%u", (unsigned)req->content_len);
    while (total < req->content_len) {
        const int received = httpd_req_recv(req, out + total, req->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW("nilan_onboarding", "body timeout after %u bytes", (unsigned)total);
            if (++timeout_retries > 20U) return false;
            continue;
        }
        if (received <= 0) return false;
        total += (size_t)received;
    }
    out[total] = '\0';
    return true;
}

static bool claim_valid(const char *candidate)
{
    return candidate != NULL && s_claim_token[0] != '\0' &&
           esp_timer_get_time() < s_claim_expires_us &&
           strcmp(candidate, s_claim_token) == 0;
}

static void ensure_claim_token(void)
{
    if (claim_valid(s_claim_token)) return;
    (void)snprintf(s_claim_token, sizeof(s_claim_token), "%06" PRIu32,
                   (uint32_t)(esp_random() % 1000000U));
    s_claim_expires_us = (uint64_t)esp_timer_get_time() + CLAIM_TOKEN_LIFETIME_US;
}

static bool bearer_matches(httpd_req_t *req)
{
    char header[TOKEN_MAX_LEN] = {0};
    char stored[TOKEN_MAX_LEN] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return false;
    if (strncmp(header, "Bearer ", 7) != 0 || !nvs_string("ob_admin_token", stored, sizeof(stored))) return false;
    return stored[0] != '\0' && strcmp(header + 7, stored) == 0;
}

bool nilan_onboarding_authorized(httpd_req_t *req)
{
    if (bearer_matches(req)) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_sendstr(req, "{\"error\":\"authorization required\"}");
    return false;
}

void nilan_onboarding_init(nilan_onboarding_reload_mqtt_fn reload_mqtt)
{
    s_reload_mqtt = reload_mqtt;
    s_claim_token[0] = '\0';
    s_claim_expires_us = 0U;
}

esp_err_t nilan_identity_get_handler(httpd_req_t *req)
{
    char id[80];
    device_id(id, sizeof(id));
    const esp_app_desc_t *app = esp_app_get_description();
    char body[640];
    (void)snprintf(body, sizeof(body),
                   "{\"device_id\":\"%s\",\"firmware_version\":\"%s\","
                   "\"hardware\":\"esp32-s3r8\",\"product\":\"nilan-comfort-302-cts602\","
                   "\"capabilities\":[\"nilan-cts602\",\"mqtt\",\"homie-v5\",\"ota\",\"rs485\"]}",
                   id, app != NULL ? app->version : "unknown");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t nilan_claim_token_get_handler(httpd_req_t *req)
{
    char state[32];
    onboarding_state(state, sizeof(state));
    if (strcmp(state, "unclaimed") != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"device already claimed\"}");
    }
    ensure_claim_token();
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint32_t expires = s_claim_expires_us > now ? (uint32_t)((s_claim_expires_us - now) / 1000000ULL) : 0U;
    char id[80], body[240];
    device_id(id, sizeof(id));
    (void)snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"claim_token\":\"%s\",\"expires_in_s\":%" PRIu32 "}", id, s_claim_token, expires);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t nilan_onboarding_status_get_handler(httpd_req_t *req)
{
    char state[32], edge_url[160];
    onboarding_state(state, sizeof(state));
    (void)nvs_string("edge_url", edge_url, sizeof(edge_url));
    char id[80], body[420];
    device_id(id, sizeof(id));
    (void)snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"state\":\"%s\",\"mqtt_configured\":%s,\"edge_url_configured\":%s}",
                   id, state, strcmp(state, "unclaimed") != 0 ? "true" : "false", edge_url[0] != '\0' ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t nilan_onboarding_configure_post_handler(httpd_req_t *req)
{
    memset(s_onboarding_body, 0, sizeof(s_onboarding_body));
    char *body = s_onboarding_body;
    if (!read_body(req, body, sizeof(s_onboarding_body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid request body\"}");
    }

    char state[32], claim[16] = {0}, admin[TOKEN_MAX_LEN] = {0};
    onboarding_state(state, sizeof(state));
    const bool has_claim = json_string(body, "claim_token", claim, sizeof(claim));
    const bool has_admin = json_string(body, "device_admin_token", admin, sizeof(admin));
    if (strcmp(state, "unclaimed") == 0) {
        ensure_claim_token();
        if (!has_claim || !claim_valid(claim)) {
            httpd_resp_set_status(req, "403 Forbidden");
            return httpd_resp_sendstr(req, "{\"error\":\"invalid claim token\"}");
        }
    } else if (!has_admin || !bearer_matches(req) || !nvs_string("ob_admin_token", state, sizeof(state)) || strcmp(admin, state) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid device admin token\"}");
    }

    char edge_url[160] = {0}, mqtt_uri[160] = {0}, mqtt_user[96] = {0}, mqtt_password[128] = {0}, mqtt_base[96] = {0}, id[80];
    int32_t domain = 0, site = 0;
    if (!json_string(body, "edge_url", edge_url, sizeof(edge_url)) ||
        !json_string(body, "mqtt_uri", mqtt_uri, sizeof(mqtt_uri)) ||
        !json_string(body, "mqtt_username", mqtt_user, sizeof(mqtt_user)) ||
        !json_string(body, "mqtt_password", mqtt_password, sizeof(mqtt_password)) ||
        !json_string(body, "mqtt_base", mqtt_base, sizeof(mqtt_base)) ||
        !json_i32(body, "domain_id", &domain) || !json_i32(body, "site_id", &site) || domain < 1 || site < 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"missing required onboarding fields\"}");
    }
    device_id(id, sizeof(id));
    nvs_handle_t nvs = 0;
    if (nvs_open("cfg", NVS_READWRITE, &nvs) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"nvs open failed\"}");
    }
    esp_err_t err = nvs_set_str(nvs, "ob_state", "claimed");
    if (err == ESP_OK) err = nvs_set_str(nvs, "edge_url", edge_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_uri", mqtt_uri);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_username", mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_password", mqtt_password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_client_id", id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_base", mqtt_base);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "mqtt_enabled", 1U);
    if (err == ESP_OK && has_admin) err = nvs_set_str(nvs, "ob_admin_token", admin);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "ob_domain", (uint32_t)domain);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "ob_site", (uint32_t)site);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"nvs commit failed\"}");
    }
    s_claim_token[0] = '\0';
    char response[260];
    (void)snprintf(response, sizeof(response), "{\"ok\":true,\"state\":\"claimed\",\"device_id\":\"%s\",\"mqtt_reload\":\"scheduled\"}", id);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t response_err = httpd_resp_sendstr(req, response);
    if (response_err == ESP_OK && s_reload_mqtt != NULL) {
        if (xTaskCreate(mqtt_reload_task, "nilan_mqtt_reload", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW("nilan_onboarding", "Could not schedule MQTT reload after onboarding");
        }
    }
    return response_err;
}

esp_err_t nilan_onboarding_reset_post_handler(httpd_req_t *req)
{
    if (!nilan_onboarding_authorized(req)) return ESP_OK;
    nvs_handle_t nvs = 0;
    if (nvs_open("cfg", NVS_READWRITE, &nvs) != ESP_OK) return ESP_FAIL;
    (void)nvs_erase_key(nvs, "ob_state");
    (void)nvs_erase_key(nvs, "edge_url");
    (void)nvs_erase_key(nvs, "ob_admin_token");
    (void)nvs_erase_key(nvs, "mqtt_enabled");
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    s_claim_token[0] = '\0';
    return httpd_resp_sendstr(req, "{\"ok\":true,\"state\":\"unclaimed\"}");
}
