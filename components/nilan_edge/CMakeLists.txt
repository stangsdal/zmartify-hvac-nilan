idf_component_register(
    SRCS "src/nilan_edge.c" "src/nilan_onboarding.c"
    INCLUDE_DIRS "include"
    REQUIRES nilan_cts602 ota net esp_http_server espressif__mqtt nvs_flash esp_wifi esp_netif esp-tls esp_app_format esp_system
)
