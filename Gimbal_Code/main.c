#define F_CPU 16000000UL

#include <avr/io.h>
#include <stdint.h>
#include <stdio.h>
#include <util/delay.h>

#include "uart.h"

#if defined(TWBR0)
#define TWI_BR      TWBR0
#define TWI_SR      TWSR0
#define TWI_AR      TWAR0
#define TWI_DR      TWDR0
#define TWI_CR      TWCR0
#else
#define TWI_BR      TWBR
#define TWI_SR      TWSR
#define TWI_AR      TWAR
#define TWI_DR      TWDR
#define TWI_CR      TWCR
#endif

#define LSM6DS0_ADDR_LOW             0x6AU
#define LSM6DS0_ADDR_HIGH            0x6BU

#define LSM6DS0_REG_WHO_AM_I         0x0FU
#define LSM6DS0_REG_CTRL1_XL         0x10U
#define LSM6DS0_REG_CTRL2_G          0x11U
#define LSM6DS0_REG_CTRL3_C          0x12U
#define LSM6DS0_REG_OUTX_L_G         0x22U

#define LSM6DS0_CTRL3_BDU_IFINC      0x44U
#define LSM6DS0_CTRL1_XL_104HZ_2G    0x40U
#define LSM6DS0_CTRL2_G_104HZ_245DPS 0x40U

#define TWI_READ                     1U
#define TWI_WRITE                    0U

#define TWI_STATUS_MASK              0xF8U
#define TWI_START_OK                 0x08U
#define TWI_REP_START_OK             0x10U
#define TWI_SLA_W_ACK                0x18U
#define TWI_SLA_R_ACK                0x40U
#define TWI_DATA_TX_ACK              0x28U

typedef struct
{
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t ax;
    int16_t ay;
    int16_t az;
} lsm6ds0_sample_t;

static uint8_t g_lsm6ds0_addr = LSM6DS0_ADDR_LOW;

static uint8_t twi_status(void)
{
    return (uint8_t)(TWI_SR & TWI_STATUS_MASK);
}

static void twi_init(void)
{
    /* 100kHz SCL with F_CPU=16MHz, prescaler=1 */
    TWI_SR &= (uint8_t)~((1U << TWPS0) | (1U << TWPS1));
    TWI_BR = 72U;
    TWI_AR = 0x00U;
    TWI_CR = (1U << TWEN);
}

static uint8_t twi_start(uint8_t address_rw)
{
    uint8_t status;

    TWI_CR = (1U << TWINT) | (1U << TWSTA) | (1U << TWEN);
    while ((TWI_CR & (1U << TWINT)) == 0U)
    {
    }

    status = twi_status();
    if ((status != TWI_START_OK) && (status != TWI_REP_START_OK))
    {
        return 0U;
    }

    TWI_DR = address_rw;
    TWI_CR = (1U << TWINT) | (1U << TWEN);
    while ((TWI_CR & (1U << TWINT)) == 0U)
    {
    }

    status = twi_status();
    if ((address_rw & 0x01U) != 0U)
    {
        return (status == TWI_SLA_R_ACK);
    }
    return (status == TWI_SLA_W_ACK);
}

static void twi_stop(void)
{
    TWI_CR = (1U << TWINT) | (1U << TWSTO) | (1U << TWEN);
}

static uint8_t twi_write(uint8_t value)
{
    TWI_DR = value;
    TWI_CR = (1U << TWINT) | (1U << TWEN);
    while ((TWI_CR & (1U << TWINT)) == 0U)
    {
    }

    return (twi_status() == TWI_DATA_TX_ACK);
}

static uint8_t twi_read_ack(void)
{
    TWI_CR = (1U << TWINT) | (1U << TWEN) | (1U << TWEA);
    while ((TWI_CR & (1U << TWINT)) == 0U)
    {
    }
    return TWI_DR;
}

static uint8_t twi_read_nack(void)
{
    TWI_CR = (1U << TWINT) | (1U << TWEN);
    while ((TWI_CR & (1U << TWINT)) == 0U)
    {
    }
    return TWI_DR;
}

static uint8_t lsm6ds0_write_reg(uint8_t reg, uint8_t value)
{
    if (!twi_start((uint8_t)((g_lsm6ds0_addr << 1) | TWI_WRITE)))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_write(reg))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_write(value))
    {
        twi_stop();
        return 0U;
    }

    twi_stop();
    return 1U;
}

static uint8_t lsm6ds0_read_reg(uint8_t reg, uint8_t *value)
{
    if (!twi_start((uint8_t)((g_lsm6ds0_addr << 1) | TWI_WRITE)))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_write(reg))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_start((uint8_t)((g_lsm6ds0_addr << 1) | TWI_READ)))
    {
        twi_stop();
        return 0U;
    }

    *value = twi_read_nack();
    twi_stop();
    return 1U;
}

static uint8_t lsm6ds0_read_bytes(uint8_t start_reg, uint8_t *buffer, uint8_t length)
{
    uint8_t i;

    if (!twi_start((uint8_t)((g_lsm6ds0_addr << 1) | TWI_WRITE)))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_write(start_reg))
    {
        twi_stop();
        return 0U;
    }

    if (!twi_start((uint8_t)((g_lsm6ds0_addr << 1) | TWI_READ)))
    {
        twi_stop();
        return 0U;
    }

    for (i = 0U; i < length; i++)
    {
        if (i < (uint8_t)(length - 1U))
        {
            buffer[i] = twi_read_ack();
        }
        else
        {
            buffer[i] = twi_read_nack();
        }
    }

    twi_stop();
    return 1U;
}

static int16_t make_s16(uint8_t low, uint8_t high)
{
    return (int16_t)(((uint16_t)high << 8) | low);
}

static void print_thousandths(int32_t value)
{
    uint32_t mag;

    if (value < 0)
    {
        putchar('-');
        mag = (uint32_t)(-value);
    }
    else
    {
        mag = (uint32_t)value;
    }

    printf("%lu.%03lu", (unsigned long)(mag / 1000UL), (unsigned long)(mag % 1000UL));
}

static uint8_t whoami_is_known(uint8_t whoami)
{
    return (uint8_t)((whoami == 0x68U) ||
                     (whoami == 0x69U) ||
                     (whoami == 0x6AU) ||
                     (whoami == 0x6BU) ||
                     (whoami == 0x6CU));
}

static uint8_t lsm6ds0_detect(uint8_t *whoami)
{
    uint8_t detected;

    g_lsm6ds0_addr = LSM6DS0_ADDR_LOW;
    detected = lsm6ds0_read_reg(LSM6DS0_REG_WHO_AM_I, whoami);
    if (detected)
    {
        return 1U;
    }

    g_lsm6ds0_addr = LSM6DS0_ADDR_HIGH;
    detected = lsm6ds0_read_reg(LSM6DS0_REG_WHO_AM_I, whoami);
    if (detected)
    {
        return 1U;
    }

    return 0U;
}

static uint8_t lsm6ds0_init(void)
{
    if (!lsm6ds0_write_reg(LSM6DS0_REG_CTRL3_C, LSM6DS0_CTRL3_BDU_IFINC))
    {
        return 0U;
    }

    if (!lsm6ds0_write_reg(LSM6DS0_REG_CTRL1_XL, LSM6DS0_CTRL1_XL_104HZ_2G))
    {
        return 0U;
    }

    if (!lsm6ds0_write_reg(LSM6DS0_REG_CTRL2_G, LSM6DS0_CTRL2_G_104HZ_245DPS))
    {
        return 0U;
    }

    _delay_ms(20);
    return 1U;
}

static uint8_t lsm6ds0_read_sample(lsm6ds0_sample_t *sample)
{
    uint8_t raw[12];

    if (!lsm6ds0_read_bytes(LSM6DS0_REG_OUTX_L_G, raw, sizeof(raw)))
    {
        return 0U;
    }

    sample->gx = make_s16(raw[0], raw[1]);
    sample->gy = make_s16(raw[2], raw[3]);
    sample->gz = make_s16(raw[4], raw[5]);
    sample->ax = make_s16(raw[6], raw[7]);
    sample->ay = make_s16(raw[8], raw[9]);
    sample->az = make_s16(raw[10], raw[11]);
    return 1U;
}

static void print_sample(const lsm6ds0_sample_t *sample)
{
    int32_t ax_mg_thousandths;
    int32_t ay_mg_thousandths;
    int32_t az_mg_thousandths;
    int32_t gx_dps_thousandths;
    int32_t gy_dps_thousandths;
    int32_t gz_dps_thousandths;

    /* FS=2g gives 0.061 mg/LSB, FS=245dps gives 0.00875 dps/LSB on LSM6-class parts */
    ax_mg_thousandths = (int32_t)sample->ax * 61L;
    ay_mg_thousandths = (int32_t)sample->ay * 61L;
    az_mg_thousandths = (int32_t)sample->az * 61L;

    gx_dps_thousandths = ((int32_t)sample->gx * 875L) / 100L;
    gy_dps_thousandths = ((int32_t)sample->gy * 875L) / 100L;
    gz_dps_thousandths = ((int32_t)sample->gz * 875L) / 100L;

    printf("AX[mG]=");
    print_thousandths(ax_mg_thousandths);
    printf(" AY[mG]=");
    print_thousandths(ay_mg_thousandths);
    printf(" AZ[mG]=");
    print_thousandths(az_mg_thousandths);

    printf(" | GX[dps]=");
    print_thousandths(gx_dps_thousandths);
    printf(" GY[dps]=");
    print_thousandths(gy_dps_thousandths);
    printf(" GZ[dps]=");
    print_thousandths(gz_dps_thousandths);

    printf(" | raw A=(%d,%d,%d) G=(%d,%d,%d)\r\n",
           sample->ax,
           sample->ay,
           sample->az,
           sample->gx,
           sample->gy,
           sample->gz);
}

void Initialize()
{
    uart_init();
    twi_init();
}

int main(void)
{
    uint8_t whoami;
    uint8_t imu_ready;
    lsm6ds0_sample_t sample;

	Initialize();

	printf("\r\nATmega328PB LSM6DS0 I2C demo\r\n");
	printf("UART: 9600 baud, 8 data bits, 2 stop bits\r\n");

	imu_ready = 0U;
		
    while (1) 
    {
		if (!imu_ready)
		{
			if (!lsm6ds0_detect(&whoami))
			{
				printf("IMU not found at 0x%02X or 0x%02X. Check SDA/SCL pullups and wiring.\r\n",
				       LSM6DS0_ADDR_LOW,
				       LSM6DS0_ADDR_HIGH);
				_delay_ms(1000);
				continue;
			}

			printf("WHO_AM_I = 0x%02X at I2C addr 0x%02X\r\n", whoami, g_lsm6ds0_addr);
			if (!whoami_is_known(whoami))
			{
				printf("Warning: unexpected WHO_AM_I. Proceeding with generic LSM6 register map.\r\n");
			}

			if (!lsm6ds0_init())
			{
				printf("IMU config write failed. Retrying...\r\n");
				_delay_ms(1000);
				continue;
			}

			printf("Streaming accel/gyro at ~104 Hz (printing throttled to 20 Hz).\r\n");
			imu_ready = 1U;
		}

		if (!lsm6ds0_read_sample(&sample))
		{
			printf("IMU read failed. Reinitializing...\r\n");
			imu_ready = 0U;
			_delay_ms(200);
			continue;
		}

		print_sample(&sample);
		_delay_ms(50);
    }
}