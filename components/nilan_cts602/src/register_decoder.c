#include "nilan_cts602.h"

static int16_t signed100(uint16_t v) { return (int16_t)v; }

bool nilan_decode_state(const uint16_t *control, size_t control_count,
                        const uint16_t *ventilation, size_t ventilation_count,
                        const uint16_t *temperatures, size_t temperature_count,
                        nilan_state_t *state)
{
    if (!control || !ventilation || !temperatures || !state || control_count < 4 ||
        ventilation_count < 5 || temperature_count < 7) return false;
    state->raw[0] = control[0]; state->raw[1] = control[1];
    state->raw[2] = control[2]; state->raw[3] = control[3];
    state->run = control[0] != 0;
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
