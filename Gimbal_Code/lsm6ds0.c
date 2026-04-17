#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include <stdint.h>
#include <util/delay.h>

#include "i2c.h"
#include "lsm6ds0.h"

static const uint8_t LSM6DS0_REG_WHO_AM_I = 0x0FU;
static const uint8_t LSM6DS0_REG_CTRL1_XL = 0x10U;
static const uint8_t LSM6DS0_REG_CTRL2_G = 0x11U;
static const uint8_t LSM6DS0_REG_CTRL3_C = 0x12U;
static const uint8_t LSM6DS0_REG_OUTX_L_G = 0x22U;

static const uint8_t LSM6DS0_CTRL3_BDU_IFINC = 0x44U;
static const uint8_t LSM6DS0_CTRL1_XL_104HZ_2G = 0x40U;
static const uint8_t LSM6DS0_CTRL2_G_104HZ_245DPS = 0x40U;

static const float LSM6DS0_GYRO_DPS_PER_LSB = 0.00875f;
static const float LSM6DS0_ACCEL_DEG_PER_LSB = 0.003495f;
static const float LSM6DS0_COMPLEMENTARY_ALPHA = 0.98f;

typedef struct
{
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t ax;
    int16_t ay;
    int16_t az;
} lsm6ds0_measurements;

static void lsm6ds0_delay_ms(uint16_t delay_ms)
{
    while (delay_ms > 0U)
    {
        _delay_ms(1);
        delay_ms--;
    }
}

static int16_t lsm6ds0_make_s16(uint8_t low, uint8_t high)
{
    return (int16_t)(((uint16_t)high << 8) | low);
}

static uint8_t lsm6ds0_write_reg(const lsm6ds0_t *imu, uint8_t reg, uint8_t value)
{
    return i2c_write_reg(imu->i2c_addr, reg, value);
}

static uint8_t lsm6ds0_read_reg(const lsm6ds0_t *imu, uint8_t reg, uint8_t *value)
{
    return i2c_read_reg(imu->i2c_addr, reg, value);
}

static uint8_t lsm6ds0_read_bytes(const lsm6ds0_t *imu,
                                  uint8_t start_reg,
                                  uint8_t *buffer,
                                  uint8_t length)
{
    return i2c_read_bytes(imu->i2c_addr, start_reg, buffer, length);
}

static uint8_t lsm6ds0_read_sample(const lsm6ds0_t *imu, lsm6ds0_measurements *sample)
{
    uint8_t raw[12];

    if (!lsm6ds0_read_bytes(imu, LSM6DS0_REG_OUTX_L_G, raw, sizeof(raw)))
    {
        return 0U;
    }

    sample->gx = lsm6ds0_make_s16(raw[0], raw[1]);
    sample->gy = lsm6ds0_make_s16(raw[2], raw[3]);
    sample->gz = lsm6ds0_make_s16(raw[4], raw[5]);
    sample->ax = lsm6ds0_make_s16(raw[6], raw[7]);
    sample->ay = lsm6ds0_make_s16(raw[8], raw[9]);
    sample->az = lsm6ds0_make_s16(raw[10], raw[11]);
    return 1U;
}

void lsm6ds0_create(lsm6ds0_t *imu)
{
    imu->i2c_addr = LSM6DS0_ADDR_LOW;
    imu->gyro_x_bias_dps = 0.0f;
    imu->gyro_y_bias_dps = 0.0f;
    imu->gyro_z_bias_dps = 0.0f;
    imu->pitch_deg = 0.0f;
    imu->roll_deg = 0.0f;
    imu->initialized = 0U;
}

uint8_t lsm6ds0_detect(lsm6ds0_t *imu, uint8_t *whoami)
{
    imu->i2c_addr = LSM6DS0_ADDR_LOW;
    if (lsm6ds0_read_reg(imu, LSM6DS0_REG_WHO_AM_I, whoami))
    {
        return 1U;
    }

    imu->i2c_addr = LSM6DS0_ADDR_HIGH;
    if (lsm6ds0_read_reg(imu, LSM6DS0_REG_WHO_AM_I, whoami))
    {
        return 1U;
    }

    return 0U;
}

uint8_t lsm6ds0_is_known_whoami(uint8_t whoami) //yo lowley this is OD - do we need to check for everything in family
{
    return (uint8_t)((whoami == 0x68U) || (whoami == 0x69U) || (whoami == 0x6AU) || (whoami == 0x6BU) || (whoami == 0x6CU));
}

uint8_t lsm6ds0_init(lsm6ds0_t *imu)
{
    if (!lsm6ds0_write_reg(imu, LSM6DS0_REG_CTRL3_C, LSM6DS0_CTRL3_BDU_IFINC))
    {
        return 0U;
    }

    if (!lsm6ds0_write_reg(imu, LSM6DS0_REG_CTRL1_XL, LSM6DS0_CTRL1_XL_104HZ_2G))
    {
        return 0U;
    }

    if (!lsm6ds0_write_reg(imu, LSM6DS0_REG_CTRL2_G, LSM6DS0_CTRL2_G_104HZ_245DPS))
    {
        return 0U;
    }

    _delay_ms(20);
    return 1U;
}

uint8_t lsm6ds0_calibrate(lsm6ds0_t *imu, uint16_t samples, uint16_t sample_period_ms, lsm6ds0_attitude *attitude_out) {
    uint16_t i;
    int32_t gx_sum;
    int32_t gy_sum;
    int32_t gz_sum;
    int32_t ax_sum;
    int32_t ay_sum;
    lsm6ds0_measurements sample;
    gx_sum = 0;
    gy_sum = 0;
    gz_sum = 0;
    ax_sum = 0;
    ay_sum = 0;
    for (i = 0U; i < samples; i++)
    {
        if (!lsm6ds0_read_sample(imu, &sample)) {
            return 0U;
        }

        gx_sum += sample.gx;
        gy_sum += sample.gy;
        gz_sum += sample.gz;
        ax_sum += sample.ax;
        ay_sum += sample.ay;

        lsm6ds0_delay_ms(sample_period_ms);
    }

    imu->gyro_x_bias_dps = ((float)gx_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;
    imu->gyro_y_bias_dps = ((float)gy_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;
    imu->gyro_z_bias_dps = ((float)gz_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;

    imu->pitch_deg = -(((float)ax_sum / (float)samples) * LSM6DS0_ACCEL_DEG_PER_LSB);
    imu->roll_deg = ((float)ay_sum / (float)samples) * LSM6DS0_ACCEL_DEG_PER_LSB;
    imu->initialized = 1U;

    attitude_out->pitch_deg = imu->pitch_deg;
    attitude_out->roll_deg = imu->roll_deg;
    attitude_out->gx_dps = 0.0f;
    attitude_out->gy_dps = 0.0f;

    return 1U;
}

uint8_t lsm6ds0_update(lsm6ds0_t *imu, float dt_sec, lsm6ds0_attitude *attitude_out) {
    float acc_pitch_deg;
    float acc_roll_deg;
    float gx_dps;
    float gy_dps;
    lsm6ds0_measurements sample;
    if (!lsm6ds0_read_sample(imu, &sample)) {
        return 0U;
    }
    gx_dps = ((float)sample.gx * LSM6DS0_GYRO_DPS_PER_LSB) - imu->gyro_x_bias_dps;
    gy_dps = ((float)sample.gy * LSM6DS0_GYRO_DPS_PER_LSB) - imu->gyro_y_bias_dps;
    acc_pitch_deg = -((float)sample.ax * LSM6DS0_ACCEL_DEG_PER_LSB);
    acc_roll_deg = (float)sample.ay * LSM6DS0_ACCEL_DEG_PER_LSB;
    if (!imu->initialized) {
        imu->pitch_deg = acc_pitch_deg;
        imu->roll_deg = acc_roll_deg;
        imu->initialized = 1U;
    }
    else {
        imu->pitch_deg = (LSM6DS0_COMPLEMENTARY_ALPHA * (imu->pitch_deg + (gy_dps * dt_sec)))
                       + ((1.0f - LSM6DS0_COMPLEMENTARY_ALPHA) * acc_pitch_deg);
        imu->roll_deg = (LSM6DS0_COMPLEMENTARY_ALPHA * (imu->roll_deg + (gx_dps * dt_sec)))
                      + ((1.0f - LSM6DS0_COMPLEMENTARY_ALPHA) * acc_roll_deg);
    }

    attitude_out->pitch_deg = imu->pitch_deg;
    attitude_out->roll_deg = imu->roll_deg;
    attitude_out->gx_dps = gx_dps;
    attitude_out->gy_dps = gy_dps;

    return 1U;
}

uint8_t lsm6ds0_get_i2c_addr(const lsm6ds0_t *imu) {
    return imu->i2c_addr;
}

void lsm6ds0_get_gyro_bias_dps(const lsm6ds0_t *imu, float *gx_bias_dps,float *gy_bias_dps, float *gz_bias_dps) {
    *gx_bias_dps = imu->gyro_x_bias_dps;
    *gy_bias_dps = imu->gyro_y_bias_dps;
    *gz_bias_dps = imu->gyro_z_bias_dps;
}
