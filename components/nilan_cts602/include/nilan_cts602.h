#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#define NILAN_FIRMWARE_VERSION CONFIG_APP_PROJECT_VER
#else
#define NILAN_FIRMWARE_VERSION "host-test"
#endif
#define NILAN_BUS_VERSION "CTS602-Modbus-RTU-1"
#define NILAN_DEFAULT_SLAVE_ADDRESS 30
#define NILAN_MODBUS_BAUD 19200
#define NILAN_MODBUS_FRAME_MAX 256

typedef enum {
    NILAN_VALUE_UNAVAILABLE = 0,
    NILAN_VALUE_FRESH,
    NILAN_VALUE_STALE,
} nilan_value_status_t;

typedef enum {
    NILAN_AIRFLOW_UNSUPPORTED = 0,
    NILAN_AIRFLOW_AVAILABLE,
    NILAN_AIRFLOW_CONFIRMED,
    NILAN_AIRFLOW_EMULATED,
    NILAN_AIRFLOW_PENDING,
    NILAN_AIRFLOW_STALE,
} nilan_airflow_control_t;

typedef struct {
    uint16_t raw_version[4];
    uint16_t raw_sensors[10];
    uint16_t raw[7];
    uint16_t raw_ventilation[5];
    uint16_t raw_temperature[7];
    uint16_t raw_alarms[10];
    int16_t room_temperature_centi_c;
    int16_t inlet_temperature_centi_c;
    int16_t outlet_temperature_centi_c;
    int16_t extract_temperature_centi_c;
    int16_t humidity_centi_pct;
    uint16_t co2_ppm;
    bool room_temperature_available;
    bool humidity_available;
    bool co2_available;
    uint8_t ventilation_level;
    uint8_t actual_inlet_level;
    uint8_t actual_exhaust_level;
    uint16_t inlet_speed;
    uint16_t exhaust_speed;
    uint16_t run_set;
    uint16_t mode_set;
    uint16_t vent_set;
    uint16_t temp_set;
    uint16_t service_mode;
    uint16_t service_pct;
    bool run;
    uint16_t mode_actual;
    bool bypass_open;
    bool bypass_close;
    bool defrost_active;
    uint16_t bus_version;
    uint16_t app_version_major;
    uint16_t app_version_minor;
    uint16_t app_version_release;
    int16_t controller_board_temperature_centi_c;
    int16_t t1_intake_centi_c;
    int16_t t2_inlet_centi_c;
    int16_t t3_exhaust_centi_c;
    int16_t t4_outlet_centi_c;
    int16_t t7_inlet_centi_c;
    int16_t t8_outdoor_centi_c;
    int16_t t9_heater_centi_c;
    uint16_t filter_days_remaining;
    nilan_value_status_t status;
    uint32_t last_success_ms;
} nilan_state_t;

typedef enum {
    NILAN_CMD_SET_RUN = 0,
    NILAN_CMD_SET_MODE,
    NILAN_CMD_SET_VENTILATION,
    NILAN_CMD_SET_SETPOINT,
    NILAN_CMD_RESET_ALARM,
} nilan_command_type_t;

typedef struct {
    nilan_command_type_t type;
    int32_t value;
} nilan_command_t;

typedef struct {
    uint32_t request_count;
    uint32_t response_count;
    uint32_t timeout_count;
    uint32_t crc_error_count;
    uint32_t framing_error_count;
    uint32_t exception_count;
    uint32_t retry_count;
    bool controller_online;
    uint32_t last_success_ms;
    uint32_t write_success_count;
    uint32_t write_error_count;
    uint16_t last_write_requested;
    uint16_t last_write_readback;
    bool last_write_ok;
    uint32_t last_write_ms;
} nilan_poll_stats_t;

/* Protocol offsets from the CTS602 document, not global 3xxxx/4xxxx addresses. */
enum {
    NILAN_INPUT_VERSION_BASE = 0,
    NILAN_OUTPUT_BYPASS_BASE = 102,
    NILAN_INPUT_ALARM_BASE = 400,
    NILAN_INPUT_SENSOR_BASE = 200,
    NILAN_INPUT_CONTROL_BASE = 1000,
    NILAN_INPUT_VENTILATION_BASE = 1100,
    NILAN_INPUT_TEMPERATURE_BASE = 1200,
    NILAN_HOLDING_RUN = 1001,
    NILAN_HOLDING_MODE = 1002,
    NILAN_HOLDING_VENTILATION = 1003,
    NILAN_HOLDING_SETPOINT = 1004,
    NILAN_HOLDING_SERVICE_MODE = 1005,
    NILAN_HOLDING_SERVICE_PCT = 1006,
    NILAN_HOLDING_EXHAUST_SPEED = 200,
    NILAN_HOLDING_INLET_SPEED = 201,
    NILAN_HOLDING_FILTER_DAYS_SINCE = 3006,
};

uint16_t nilan_modbus_crc16(const uint8_t *data, size_t length);
void nilan_decode_text_word(uint16_t value, char out[3]);
size_t nilan_build_read_request(uint8_t slave, uint8_t function, uint16_t offset,
                                uint16_t quantity, uint8_t *out, size_t out_size);
size_t nilan_build_write_request(uint8_t slave, uint16_t offset, uint16_t value,
                                 uint8_t *out, size_t out_size);
bool nilan_validate_read_response(const uint8_t *frame, size_t length, uint8_t slave,
                                  uint8_t function, uint8_t *payload, size_t payload_size,
                                  size_t *payload_length);
bool nilan_validate_write_response(const uint8_t *frame, size_t length, uint8_t slave,
                                   uint16_t offset, uint16_t quantity);
bool nilan_decode_state(const uint16_t *versions, size_t version_count,
                        const uint16_t *sensors, size_t sensor_count,
                        const uint16_t *outputs, size_t output_count,
                        const uint16_t *control, size_t control_count,
                        const uint16_t *ventilation, size_t ventilation_count,
                        const uint16_t *temperatures, size_t temperature_count,
                        nilan_state_t *state);
bool nilan_validate_command(const nilan_command_t *command);
bool nilan_filter_reset_offset(uint16_t interval_days, uint16_t *out_days_since);

/* Starts the read-only CTS602 poller on an already started shared RS485 bus. */
bool nilan_adapter_start(uint8_t slave_address);
bool nilan_adapter_get_state(nilan_state_t *out_state, nilan_poll_stats_t *out_stats);
bool nilan_adapter_set_ventilation(uint8_t level, uint16_t *out_readback);
bool nilan_adapter_set_inlet_speed_pct(uint16_t pct, uint16_t *out_readback);
bool nilan_adapter_set_exhaust_speed_pct(uint16_t pct, uint16_t *out_readback);
bool nilan_adapter_set_control_register(uint16_t offset, uint16_t value, uint16_t *out_readback);
