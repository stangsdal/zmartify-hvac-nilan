#include "ota.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/md.h"

static const char *TAG = "ota";

#define OTA_RECEIVE_TIMEOUT_RETRY_MAX 12U

static bool s_initialized;
static bool s_service_started;
static ota_status_t s_status;

static void set_last_error(esp_err_t err)
{
    s_status.last_error = err;
    if (err != ESP_OK) ESP_LOGE(TAG, "OTA error: %s", esp_err_to_name(err));
}

esp_err_t ota_init(const ota_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(&s_status, 0, sizeof(s_status));
    s_status.rollback_enabled = config->rollback_enabled;
    s_status.initialized = true;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        s_status.pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ota_start_service(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_service_started = true;
    s_status.service_started = true;
    return ESP_OK;
}

esp_err_t ota_get_status(ota_status_t *out_status)
{
    if (!out_status) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    *out_status = s_status;
    return ESP_OK;
}

esp_err_t ota_handle_http_upload(httpd_req_t *req, uint32_t *out_written_bytes)
{
    if (!req || !out_written_bytes) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_service_started) return ESP_ERR_INVALID_STATE;
    if (req->content_len <= 0) {
        set_last_error(ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        set_last_error(ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Starting OTA upload: size=%d bytes, target=%s", req->content_len,
             update_partition->label);
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        set_last_error(err);
        return err;
    }

    uint8_t buf[1024];
    uint32_t written = 0;
    uint32_t timeout_retries = 0;
    while (written < (uint32_t)req->content_len) {
        const int to_read = (int)(((uint32_t)req->content_len - written) > sizeof(buf)
                                      ? sizeof(buf)
                                      : ((uint32_t)req->content_len - written));
        const int read_len = httpd_req_recv(req, (char *)buf, to_read);
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT &&
            timeout_retries < OTA_RECEIVE_TIMEOUT_RETRY_MAX) {
            ++timeout_retries;
            ESP_LOGW(TAG, "OTA receive timeout (%u/%u); retrying",
                     (unsigned)timeout_retries, (unsigned)OTA_RECEIVE_TIMEOUT_RETRY_MAX);
            continue;
        }
        if (read_len <= 0) {
            (void)esp_ota_abort(ota_handle);
            set_last_error(ESP_FAIL);
            return ESP_FAIL;
        }

        timeout_retries = 0;
        err = esp_ota_write(ota_handle, buf, (size_t)read_len);
        if (err != ESP_OK) {
            (void)esp_ota_abort(ota_handle);
            set_last_error(err);
            return err;
        }
        written += (uint32_t)read_len;
        if ((written % (32U * 1024U)) == 0 || written == (uint32_t)req->content_len) {
            ESP_LOGI(TAG, "OTA progress: %u/%u bytes", (unsigned)written,
                     (unsigned)req->content_len);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        set_last_error(err);
        return err;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        set_last_error(err);
        return err;
    }

    s_status.last_update_ok = true;
    s_status.last_update_bytes = written;
    s_status.pending_verify = true;
    set_last_error(ESP_OK);
    *out_written_bytes = written;
    ESP_LOGI(TAG, "OTA upload finished; boot partition switched to %s", update_partition->label);
    return ESP_OK;
}

esp_err_t ota_finalize_boot_validation(bool boot_healthy)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
        s_status.pending_verify = false;
        return ESP_OK;
    }
    if (err != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return err;

    if (boot_healthy) {
        ESP_LOGI(TAG, "Boot health checks passed; marking running app valid");
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) s_status.pending_verify = false;
        return err;
    }

    ESP_LOGE(TAG, "Boot health checks failed; triggering rollback");
    s_status.last_update_ok = false;
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t ota_pull_and_apply(const char *download_url, const char *bearer_token,
                             const char *expected_sha256, uint32_t *out_written_bytes)
{
    if (!download_url || !bearer_token || !expected_sha256 || !out_written_bytes || strlen(expected_sha256) != 64U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_written_bytes = 0U;
    esp_http_client_config_t cfg = {.url = download_url, .method = HTTP_METHOD_GET,
                                    .timeout_ms = 30000, .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    char auth[192];
    (void)snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
    (void)esp_http_client_set_header(client, "Authorization", auth);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
    if (esp_http_client_fetch_headers(client) < 0 || esp_http_client_get_status_code(client) < 200 || esp_http_client_get_status_code(client) >= 300) {
        esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_ERR_NOT_FOUND; }
    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) { esp_http_client_close(client); esp_http_client_cleanup(client); return err; }
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 || mbedtls_md_starts(&md) != 0) {
        mbedtls_md_free(&md); (void)esp_ota_abort(handle); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
    }
    uint8_t buf[1024], digest[32];
    uint32_t written = 0U;
    for (;;) {
        const int n = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (n < 0) { (void)esp_ota_abort(handle); mbedtls_md_free(&md); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL; }
        if (n == 0) break;
        if (esp_ota_write(handle, buf, (size_t)n) != ESP_OK || mbedtls_md_update(&md, buf, (size_t)n) != 0) {
            (void)esp_ota_abort(handle); mbedtls_md_free(&md); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
        }
        written += (uint32_t)n;
    }
    if (mbedtls_md_finish(&md, digest) != 0) {
        (void)esp_ota_abort(handle); mbedtls_md_free(&md); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL;
    }
    mbedtls_md_free(&md);
    char actual[65];
    for (size_t i = 0; i < sizeof(digest); ++i) (void)snprintf(actual + i * 2U, 3U, "%02x", digest[i]);
    if (strcmp(actual, expected_sha256) != 0) {
        (void)esp_ota_abort(handle); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_ERR_INVALID_CRC;
    }
    err = esp_ota_end(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (err == ESP_OK) *out_written_bytes = written;
    return err;
}
