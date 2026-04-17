#ifndef I2C_H
#define I2C_H

#include <stdint.h>

void i2c_init(void);

uint8_t i2c_write_reg(uint8_t device_addr, uint8_t reg, uint8_t value);
uint8_t i2c_read_reg(uint8_t device_addr, uint8_t reg, uint8_t *value);
uint8_t i2c_read_bytes(uint8_t device_addr, uint8_t start_reg, uint8_t *buffer, uint8_t length);

#endif
