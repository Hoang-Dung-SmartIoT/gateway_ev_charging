#pragma once
#include <stdint.h>

uint16_t modbus_crc16(const uint8_t *buf, uint16_t len);
