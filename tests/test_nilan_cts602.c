#include "nilan_cts602.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_requests(void)
{
    uint8_t frame[NILAN_MODBUS_FRAME_MAX];
    assert(nilan_build_read_request(30, 4, 1000, 4, frame, sizeof(frame)) == 8);
    assert(memcmp(frame, (uint8_t[]){30, 4, 3, 232, 0, 4}, 6) == 0);
    assert(nilan_modbus_crc16(frame, 6) == ((uint16_t)frame[6] | ((uint16_t)frame[7] << 8)));
    assert(nilan_build_write_request(30, 1003, 4, frame, sizeof(frame)) == 11);
    assert(frame[1] == 16 && frame[2] == 3 && frame[3] == 235 && frame[8] == 4);
    assert(nilan_build_read_request(30, 4, 102, 2, frame, sizeof(frame)) == 8);
    assert(frame[1] == 4 && frame[2] == 0 && frame[3] == 102 && frame[5] == 2);
    assert(nilan_build_read_request(30, 1, 102, 2, frame, sizeof(frame)) == 0);
}

static void test_response_and_decode(void)
{
    uint8_t frame[] = {30, 4, 4, 0, 1, 0, 2};
    uint16_t crc = nilan_modbus_crc16(frame, sizeof(frame));
    uint8_t with_crc[9]; memcpy(with_crc, frame, sizeof(frame));
    with_crc[7] = crc; with_crc[8] = crc >> 8;
    uint8_t payload[8]; size_t payload_len = 0;
    assert(nilan_validate_read_response(with_crc, sizeof(with_crc), 30, 4,
                                        payload, sizeof(payload), &payload_len));
    assert(payload_len == 4 && payload[1] == 1);

    char version_part[3];
    nilan_decode_text_word(0x2e32, version_part);
    assert(strcmp(version_part, "2.") == 0);
    uint16_t versions[] = {21, 0x2e32, 0x3733, 0x632e};
    uint16_t sensors[] = {3150, (uint16_t)(int16_t)-500, 80, 2100, 1200, 0, 0, 1900, 750, 2300};
    uint16_t outputs[] = {1, 0};
    uint16_t control[] = {1, 3, 2, 0};
    uint16_t ventilation[] = {3, 4, 2, 0, 72};
    uint16_t temperatures[] = {2150, 80, 1200, 2100, 4800, 650, 0};
    nilan_state_t state = {0};
    assert(nilan_decode_state(versions, 4, sensors, 10, outputs, 2,
                              control, 4, ventilation, 5, temperatures, 7, &state));
    assert(state.run && state.room_temperature_centi_c == 2150);
    assert(state.mode_actual == 3 && state.bypass_open && !state.bypass_close);
    assert(state.bus_version == 21 && state.app_version_major == 0x2e32);
    assert(state.controller_board_temperature_centi_c == 3150);
    assert(state.t1_intake_centi_c == -500 && state.t7_inlet_centi_c == 1900);
    assert(state.t8_outdoor_centi_c == 750 && state.t9_heater_centi_c == 2300);
    assert(state.room_temperature_available && state.co2_available);
    assert(state.humidity_available && state.humidity_centi_pct == 4800);
    assert(state.actual_inlet_level == 4 && state.filter_days_remaining == 72);
    temperatures[0] = 1;
    temperatures[5] = 55536;
    assert(nilan_decode_state(versions, 4, sensors, 10, outputs, 2,
                              control, 4, ventilation, 5, temperatures, 7, &state));
    assert(!state.room_temperature_available && !state.co2_available);
    temperatures[0] = (uint16_t)(int16_t)-500;
    temperatures[5] = 650;
    assert(nilan_decode_state(versions, 4, sensors, 10, outputs, 2,
                              control, 4, ventilation, 5, temperatures, 7, &state));
    assert(state.room_temperature_available && state.room_temperature_centi_c == -500);
    temperatures[4] = (uint16_t)(int16_t)-26111;
    assert(nilan_decode_state(versions, 4, sensors, 10, outputs, 2,
                              control, 4, ventilation, 5, temperatures, 7, &state));
    assert(!state.humidity_available);
}

static void test_guards(void)
{
    nilan_command_t command = {NILAN_CMD_SET_VENTILATION, 4};
    assert(nilan_validate_command(&command));
    command.value = 5; assert(!nilan_validate_command(&command));
    command.type = NILAN_CMD_SET_SETPOINT; command.value = 3001;
    assert(!nilan_validate_command(&command));
}

static void test_filter_reset_offsets(void)
{
    uint16_t days_since = 0;
    assert(nilan_filter_reset_offset(183, &days_since) && days_since == 182);
    assert(nilan_filter_reset_offset(274, &days_since) && days_since == 91);
    assert(nilan_filter_reset_offset(365, &days_since) && days_since == 0);
    assert(!nilan_filter_reset_offset(200, &days_since));
}

int main(void)
{
    test_requests(); test_response_and_decode(); test_guards(); test_filter_reset_offsets();
    puts("nilan_cts602 host tests: ok");
    return 0;
}
