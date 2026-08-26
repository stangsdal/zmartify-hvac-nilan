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

    uint16_t control[] = {1, 0, 2, 0};
    uint16_t ventilation[] = {3, 4, 2, 0, 72};
    uint16_t temperatures[] = {2150, 80, 1200, 2100, 4800, 650, 0};
    nilan_state_t state = {0};
    assert(nilan_decode_state(control, 4, ventilation, 5, temperatures, 7, &state));
    assert(state.run && state.room_temperature_centi_c == 2150);
    assert(state.actual_inlet_level == 4 && state.filter_days_remaining == 72);
}

static void test_guards(void)
{
    nilan_command_t command = {NILAN_CMD_SET_VENTILATION, 4};
    assert(nilan_validate_command(&command));
    command.value = 5; assert(!nilan_validate_command(&command));
    command.type = NILAN_CMD_SET_SETPOINT; command.value = 3001;
    assert(!nilan_validate_command(&command));
}

int main(void)
{
    test_requests(); test_response_and_decode(); test_guards();
    puts("nilan_cts602 host tests: ok");
    return 0;
}
