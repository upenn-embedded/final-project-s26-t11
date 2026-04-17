#include <avr/io.h>
#include <stdint.h>

#include "i2c.h"

#if defined(TWBR0)
#define I2C_BR TWBR0
#define I2C_SR TWSR0
#define I2C_AR TWAR0
#define I2C_DR TWDR0
#define I2C_CR TWCR0
#else
#define I2C_BR TWBR
#define I2C_SR TWSR
#define I2C_AR TWAR
#define I2C_DR TWDR
#define I2C_CR TWCR
#endif

#define I2C_READ 1U
#define I2C_WRITE 0U

#define I2C_STATUS_MASK 0xF8U
#define I2C_START_OK 0x08U
#define I2C_REP_START_OK 0x10U
#define I2C_SLA_W_ACK 0x18U
#define I2C_SLA_R_ACK 0x40U
#define I2C_DATA_TX_ACK 0x28U

static uint8_t i2c_status(void) {
    return (uint8_t)(I2C_SR & I2C_STATUS_MASK);
}

void i2c_init(void) {
    /* 100kHz SCL, F_CPU=16MHz,prescaler of 1 */
    I2C_SR &= (uint8_t)~((1U << TWPS0) | (1U << TWPS1));
    I2C_BR = 72U;
    I2C_AR = 0x00U;
    I2C_CR = (1U << TWEN);
}

static uint8_t i2c_start(uint8_t address_rw) {
    uint8_t status;

    I2C_CR = (1U << TWINT) | (1U << TWSTA) | (1U << TWEN);
    while ((I2C_CR & (1U << TWINT)) == 0U)
    {
    }

    status = i2c_status();
    if ((status != I2C_START_OK) && (status != I2C_REP_START_OK))
    {
        return 0U;
    }

    I2C_DR = address_rw;
    I2C_CR = (1U << TWINT) | (1U << TWEN);
    while ((I2C_CR & (1U << TWINT)) == 0U)
    {
    }

    status = i2c_status();
    if ((address_rw & 0x01U) != 0U)
    {
        return (status == I2C_SLA_R_ACK);
    }
    return (status == I2C_SLA_W_ACK);
}

static void i2c_stop(void)
{
    I2C_CR = (1U << TWINT) | (1U << TWSTO) | (1U << TWEN);
}

static uint8_t i2c_write(uint8_t value)
{
    I2C_DR = value;
    I2C_CR = (1U << TWINT) | (1U << TWEN);
    while ((I2C_CR & (1U << TWINT)) == 0U)
    {
    }

    return (i2c_status() == I2C_DATA_TX_ACK);
}

static uint8_t i2c_read_ack(void)
{
    I2C_CR = (1U << TWINT) | (1U << TWEN) | (1U << TWEA);
    while ((I2C_CR & (1U << TWINT)) == 0U)
    {
    }
    return I2C_DR;
}

static uint8_t i2c_read_nack(void)
{
    I2C_CR = (1U << TWINT) | (1U << TWEN);
    while ((I2C_CR & (1U << TWINT)) == 0U)
    {
    }
    return I2C_DR;
}

uint8_t i2c_write_reg(uint8_t device_addr, uint8_t reg, uint8_t value)
{
    if (!i2c_start((uint8_t)((device_addr << 1) | I2C_WRITE)))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_write(reg))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_write(value))
    {
        i2c_stop();
        return 0U;
    }

    i2c_stop();
    return 1U;
}

uint8_t i2c_read_reg(uint8_t device_addr, uint8_t reg, uint8_t *value)
{
    if (!i2c_start((uint8_t)((device_addr << 1) | I2C_WRITE)))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_write(reg))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_start((uint8_t)((device_addr << 1) | I2C_READ)))
    {
        i2c_stop();
        return 0U;
    }

    *value = i2c_read_nack();
    i2c_stop();
    return 1U;
}

uint8_t i2c_read_bytes(uint8_t device_addr, uint8_t start_reg, uint8_t *buffer, uint8_t length)
{
    uint8_t i;

    if (!i2c_start((uint8_t)((device_addr << 1) | I2C_WRITE)))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_write(start_reg))
    {
        i2c_stop();
        return 0U;
    }

    if (!i2c_start((uint8_t)((device_addr << 1) | I2C_READ)))
    {
        i2c_stop();
        return 0U;
    }

    for (i = 0U; i < length; i++)
    {
        if (i < (uint8_t)(length - 1U))
        {
            buffer[i] = i2c_read_ack();
        }
        else
        {
            buffer[i] = i2c_read_nack();
        }
    }

    i2c_stop();
    return 1U;
}
