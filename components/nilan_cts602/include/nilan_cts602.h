#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NILAN_FIRMWARE_VERSION "0.1.0"
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
    uint16_t raw[7];
    int16_t room_temperature_centi_c;
    int16_t inlet_temperature_centi_c;
    int16_t outlet_temperature_centi_c;
    int16_t extract_temperature_centi_c;
    int16_t humidity_centi_pct;
    uint16_t co2_ppm;
    uint8_t ventilation_level;
    uint8_t actual_inlet_level;
    uint8_t actual_exhaust_level;
    bool run;
    bool bypass_open;
    bool defrost_active;
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
} nilan_poll_stats_t;

/* Protocol offsets from the CTS602 document, not global 3xxxx/4xxxx addresses. */
enum {
    NILAN_INPUT_ALARM_BASE = 400,
    NILAN_INPUT_SENSOR_BASE = 200,
    NILAN_INPUT_CONTROL_BASE = 1000,
    NILAN_INPUT_VENTILATION_BASE = 1100,
    NILAN_INPUT_TEMPERATURE_BASE = 1200,
    NILAN_HOLDING_RUN = 1001,
    NILAN_HOLDING_MODE = 1002,
    NILAN_HOLDING_VENTILATION = 1003,
    NILAN_HOLDING_SETPOINT = 1004,
};

uint16_t nilan_modbus_crc16(const uint8_t *data, size_t length);
size_t nilan_build_read_request(uint8_t slave, uint8_t function, uint16_t offset,
                                uint16_t quantity, uint8_t *out, size_t out_size);
size_t nilan_build_write_request(uint8_t slave, uint16_t offset, uint16_t value,
                                 uint8_t *out, size_t out_size);
bool nilan_validate_read_response(const uint8_t *frame, size_t length, uint8_t slave,
                                  uint8_t function, uint8_t *payload, size_t payload_size,
                                  size_t *payload_length);
bool nilan_decode_state(const uint16_t *control, size_t control_count,
                        const uint16_t *ventilation, size_t ventilation_count,
                        const uint16_t *temperatures, size_t temperature_count,
                        nilan_state_t *state);
bool nilan_validate_command(const nilan_command_t *command);

/* Starts the read-only CTS602 poller on an already started shared RS485 bus. */
bool nilan_adapter_start(uint8_t slave_address);
bool nilan_adapter_get_state(nilan_state_t *out_state, nilan_poll_stats_t *out_stats);
