idf_component_register(
    SRCS "src/nilan_cts602.c" "src/modbus_rtu.c" "src/register_decoder.c" "src/adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES comm_rs485 freertos esp_timer
)
