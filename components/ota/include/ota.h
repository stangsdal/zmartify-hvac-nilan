#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct httpd_req httpd_req_t;

typedef struct {
    bool rollback_enabled;
} ota_config_t;

typedef struct {
    bool initialized;
    bool service_started;
    bool rollback_enabled;
    bool pending_verify;
    bool last_update_ok;
    uint32_t last_update_bytes;
    esp_err_t last_error;
} ota_status_t;

esp_err_t ota_init(const ota_config_t *config);
esp_err_t ota_start_service(void);
esp_err_t ota_get_status(ota_status_t *out_status);
esp_err_t ota_handle_http_upload(httpd_req_t *req, uint32_t *out_written_bytes);
esp_err_t ota_finalize_boot_validation(bool boot_healthy);
esp_err_t ota_pull_and_apply(const char *download_url, const char *bearer_token,
                             const char *expected_sha256, uint32_t *out_written_bytes);
