#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef bool (*nilan_onboarding_reload_mqtt_fn)(void);

void nilan_onboarding_init(nilan_onboarding_reload_mqtt_fn reload_mqtt);
bool nilan_onboarding_authorized(httpd_req_t *req);

esp_err_t nilan_identity_get_handler(httpd_req_t *req);
esp_err_t nilan_claim_token_get_handler(httpd_req_t *req);
esp_err_t nilan_onboarding_status_get_handler(httpd_req_t *req);
esp_err_t nilan_onboarding_configure_post_handler(httpd_req_t *req);
esp_err_t nilan_onboarding_reset_post_handler(httpd_req_t *req);
