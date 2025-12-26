#pragma once
#include <stdint.h>

void modbus_rtu_init(uint8_t slave_id);
void modbus_rtu_task(void *arg);

/* User register callback */
uint16_t modbus_read_holding(uint16_t addr);
void modbus_write_holding(uint16_t addr, uint16_t value);
