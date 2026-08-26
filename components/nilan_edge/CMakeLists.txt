idf_component_register(
    SRCS "src/nilan_edge.c"
    INCLUDE_DIRS "include"
    REQUIRES nilan_cts602 esp_http_server espressif__mqtt nvs_flash esp_wifi esp_netif esp-tls
)
