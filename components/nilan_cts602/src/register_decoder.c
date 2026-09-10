#include "nilan_cts602.h"

static int16_t signed100(uint16_t v) { return (int16_t)v; }

void nilan_decode_text_word(uint16_t value, char out[3])
{
    if (!out) return;
    out[0] = (char)value;
    out[1] = (char)(value >> 8);
    out[2] = '\0';
}

bool nilan_decode_state(const uint16_t *versions, size_t version_count,
                        const uint16_t *sensors, size_t sensor_count,
                        const uint16_t *outputs, size_t output_count,
                        const uint16_t *control, size_t control_count,
                        const uint16_t *ventilation, size_t ventilation_count,
                        const uint16_t *temperatures, size_t temperature_count,
                        nilan_state_t *state)
{
    if (!versions || !sensors || !outputs || !control || !ventilation || !temperatures || !state ||
        version_count < 4 || sensor_count < 10 || output_count < 2 || control_count < 4 ||
        ventilation_count < 5 || temperature_count < 7) return false;
    for (size_t i = 0; i < 4; ++i) state->raw_version[i] = versions[i];
    for (size_t i = 0; i < 10; ++i) state->raw_sensors[i] = sensors[i];
    state->raw[0] = control[0]; state->raw[1] = control[1];
    state->raw[2] = control[2]; state->raw[3] = control[3];
    for (size_t i = 0; i < ventilation_count && i < 5; ++i) state->raw_ventilation[i] = ventilation[i];
    for (size_t i = 0; i < temperature_count && i < 7; ++i) state->raw_temperature[i] = temperatures[i];
    state->run = control[0] != 0;
    state->mode_actual = control[1];
    state->bypass_open = outputs[0] != 0;
    state->bypass_close = outputs[1] != 0;
    state->bus_version = versions[0];
    state->app_version_major = versions[1];
    state->app_version_minor = versions[2];
    state->app_version_release = versions[3];
    state->controller_board_temperature_centi_c = signed100(sensors[0]);
    state->t1_intake_centi_c = signed100(sensors[1]);
    state->t2_inlet_centi_c = signed100(sensors[2]);
    state->t3_exhaust_centi_c = signed100(sensors[3]);
    state->t4_outlet_centi_c = signed100(sensors[4]);
    state->t7_inlet_centi_c = signed100(sensors[7]);
    state->t8_outdoor_centi_c = signed100(sensors[8]);
    state->t9_heater_centi_c = signed100(sensors[9]);
    state->ventilation_level = (uint8_t)ventilation[0];
    state->actual_inlet_level = (uint8_t)ventilation[1];
    state->actual_exhaust_level = (uint8_t)ventilation[2];
    state->filter_days_remaining = ventilation[4];
    state->room_temperature_centi_c = signed100(temperatures[0]);
    state->inlet_temperature_centi_c = signed100(temperatures[1]);
    state->outlet_temperature_centi_c = signed100(temperatures[2]);
    state->extract_temperature_centi_c = signed100(temperatures[3]);
    state->humidity_centi_pct = (int16_t)temperatures[4];
    state->co2_ppm = temperatures[5];
    /* CTS602 installations may omit room and CO2 sensors. The observed raw
       values (1 and 0xd8f0) are not physical measurements. Keep the raw
       words available for protocol work, but do not publish false telemetry. */
    state->room_temperature_available = temperatures[0] != 1 &&
                                        state->room_temperature_centi_c >= -4000 &&
                                        state->room_temperature_centi_c <= 8000;
    state->humidity_available = state->humidity_centi_pct >= 0 && state->humidity_centi_pct <= 10000;
    state->co2_available = temperatures[5] <= 5000;
    state->status = NILAN_VALUE_FRESH;
    return true;
}

bool nilan_validate_command(const nilan_command_t *command)
{
    if (!command) return false;
    switch (command->type) {
    case NILAN_CMD_SET_RUN: return command->value == 0 || command->value == 1;
    case NILAN_CMD_SET_MODE: return command->value >= 0 && command->value <= 3;
    case NILAN_CMD_SET_VENTILATION: return command->value >= 0 && command->value <= 4;
    case NILAN_CMD_SET_SETPOINT: return command->value >= 1000 && command->value <= 3000;
    case NILAN_CMD_RESET_ALARM: return command->value == 255 || (command->value >= 0 && command->value <= 254);
    default: return false;
    }
}

bool nilan_filter_reset_offset(uint16_t interval_days, uint16_t *out_days_since)
{
    if (!out_days_since ||
        (interval_days != 183 && interval_days != 274 && interval_days != 365)) {
        return false;
    }
    *out_days_since = (uint16_t)(365U - interval_days);
    return true;
}
