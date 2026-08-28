#include "nilan_cts602.h"

uint16_t nilan_modbus_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static bool room(size_t actual, size_t needed) { return actual >= needed; }

size_t nilan_build_read_request(uint8_t slave, uint8_t function, uint16_t offset,
                                uint16_t quantity, uint8_t *out, size_t out_size)
{
    if (!out || out_size < 8 || (function != 3 && function != 4) || quantity == 0) return 0;
    out[0] = slave; out[1] = function; out[2] = offset >> 8; out[3] = offset;
    out[4] = quantity >> 8; out[5] = quantity;
    uint16_t crc = nilan_modbus_crc16(out, 6); out[6] = crc; out[7] = crc >> 8;
    return 8;
}

size_t nilan_build_write_request(uint8_t slave, uint16_t offset, uint16_t value,
                                 uint8_t *out, size_t out_size)
{
    if (!out || out_size < 11) return 0;
    out[0] = slave; out[1] = 16; out[2] = offset >> 8; out[3] = offset;
    out[4] = 0; out[5] = 1; out[6] = 2; out[7] = value >> 8; out[8] = value;
    uint16_t crc = nilan_modbus_crc16(out, 9); out[9] = crc; out[10] = crc >> 8;
    return 11;
}

bool nilan_validate_read_response(const uint8_t *frame, size_t length, uint8_t slave,
                                  uint8_t function, uint8_t *payload, size_t payload_size,
                                  size_t *payload_length)
{
    if (!frame || length < 5 || frame[0] != slave || frame[1] != function ||
        frame[2] != length - 5 || !room(payload_size, frame[2])) return false;
    uint16_t expected = nilan_modbus_crc16(frame, length - 2);
    uint16_t received = (uint16_t)frame[length - 2] | ((uint16_t)frame[length - 1] << 8);
    if (expected != received) return false;
    for (size_t i = 0; i < frame[2]; ++i) payload[i] = frame[3 + i];
    if (payload_length) *payload_length = frame[2];
    return true;
}

bool nilan_validate_write_response(const uint8_t *frame, size_t length, uint8_t slave,
                                   uint16_t offset, uint16_t quantity)
{
    if (!frame || length != 8 || frame[0] != slave || frame[1] != 16 ||
        frame[2] != (uint8_t)(offset >> 8) || frame[3] != (uint8_t)offset ||
        frame[4] != (uint8_t)(quantity >> 8) || frame[5] != (uint8_t)quantity) return false;
    uint16_t expected = nilan_modbus_crc16(frame, length - 2);
    uint16_t received = (uint16_t)frame[length - 2] | ((uint16_t)frame[length - 1] << 8);
    return expected == received;
}
