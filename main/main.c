#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board.h"
#include "comm_rs485.h"
#include "nilan_cts602.h"
#include "nilan_edge.h"
#include "net.h"
#include "nvs_flash.h"

static const char *TAG = "nilan";

void app_main(void)
{
    ESP_LOGI(TAG, "Zmartify Nilan CTS602 firmware %s", NILAN_FIRMWARE_VERSION);
    ESP_ERROR_CHECK(nvs_flash_init());
    const net_config_t net_cfg = {
        .wifi_ssid = NULL, .wifi_password = NULL,
        .hostname = "zmartify-hvac-nilan", .enable_ap_fallback = true,
    };
    ESP_ERROR_CHECK(net_init(&net_cfg));
    ESP_ERROR_CHECK(net_start());
    board_config_t board_cfg;
    board_get_default_config(&board_cfg);
    board_cfg.rs485_default_baud = NILAN_MODBUS_BAUD;

    const esp_err_t board_err = board_init(&board_cfg);
    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "Shared AHC9000 board init failed: %s", esp_err_to_name(board_err));
        return;
    }

    const rs485_config_t rs485_cfg = {
        .uart_num = board_cfg.rs485_uart_num,
        .tx_gpio = board_cfg.pins.rs485_tx_gpio,
        .rx_gpio = board_cfg.pins.rs485_rx_gpio,
        .de_re_gpio = board_cfg.pins.rs485_de_re_gpio,
        .baud_rate = NILAN_MODBUS_BAUD,
        .parity = RS485_PARITY_EVEN,
        .stop_bits = 1,
    };
    ESP_LOGI(TAG, "CTS602 transport: slave=%u, UART%d TX=%d RX=%d DE/RE=%d, 19200 8E1",
             NILAN_DEFAULT_SLAVE_ADDRESS, rs485_cfg.uart_num, rs485_cfg.tx_gpio,
             rs485_cfg.rx_gpio, rs485_cfg.de_re_gpio);

    const esp_err_t rs485_err = comm_rs485_init(&rs485_cfg);
    if (rs485_err != ESP_OK || comm_rs485_start() != ESP_OK) {
        ESP_LOGE(TAG, "Shared RS485 transport failed to start");
        return;
    }
    if (!nilan_adapter_start(NILAN_DEFAULT_SLAVE_ADDRESS)) {
        ESP_LOGE(TAG, "Nilan read-only adapter failed to start");
        return;
    }
    ESP_LOGI(TAG, "Nilan read-only polling started: P0 groups every 2 seconds");
    if (!nilan_edge_start()) {
        ESP_LOGE(TAG, "Nilan Edge adapter failed to start");
        return;
    }
    ESP_LOGI(TAG, "Nilan state API and MQTT publisher started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
