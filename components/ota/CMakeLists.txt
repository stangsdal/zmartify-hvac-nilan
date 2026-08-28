idf_component_register(
    SRCS "src/ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_server esp_http_client esp-tls mbedtls
)
