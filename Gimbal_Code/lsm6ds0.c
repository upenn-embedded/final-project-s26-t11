#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include <math.h>
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
static const uint8_t LSM6DS0_CTRL2_G_104HZ_500DPS = 0x44U;

static const float LSM6DS0_GYRO_DPS_PER_LSB = 0.0175f;
static const float LSM6DS0_ACCEL_LSB_PER_G = 16384.0f;
static const float LSM6DS0_DEG_TO_RAD = 0.01745329252f;
static const float LSM6DS0_RAD_TO_DEG = 57.29577951f;
static const float LSM6DS0_MAHONY_KP = 2.5f;
static const float LSM6DS0_MAHONY_KI = 0.0f;
static const float LSM6DS0_INT_LIM_RADPS = 0.2f;
static const float LSM6DS0_NORM_EPSILON = 1.0e-6f;
static const float LSM6DS0_ACCEL_TRUST_FULL_MIN_G = 0.90f;
static const float LSM6DS0_ACCEL_TRUST_FULL_MAX_G = 1.10f;
static const float LSM6DS0_ACCEL_TRUST_ZERO_MIN_G = 0.75f;
static const float LSM6DS0_ACCEL_TRUST_ZERO_MAX_G = 1.25f;
static const float LSM6DS0_GYRO_TRUST_FULL_MAX_DPS = 25.0f;
static const float LSM6DS0_GYRO_TRUST_ZERO_MIN_DPS = 140.0f;

typedef struct
{
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t ax;
    int16_t ay;
    int16_t az;
} lsm6ds0_measurements;

static float lsm6ds0_clampf(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

static float lsm6ds0_accel_trust(float accel_norm_lsb)
{
    float accel_g = accel_norm_lsb / LSM6DS0_ACCEL_LSB_PER_G;

    if ((accel_g >= LSM6DS0_ACCEL_TRUST_FULL_MIN_G) && (accel_g <= LSM6DS0_ACCEL_TRUST_FULL_MAX_G))
    {
        return 1.0f;
    }

    if ((accel_g <= LSM6DS0_ACCEL_TRUST_ZERO_MIN_G) || (accel_g >= LSM6DS0_ACCEL_TRUST_ZERO_MAX_G))
    {
        return 0.0f;
    }

    if (accel_g < LSM6DS0_ACCEL_TRUST_FULL_MIN_G)
    {
        return (accel_g - LSM6DS0_ACCEL_TRUST_ZERO_MIN_G) /
               (LSM6DS0_ACCEL_TRUST_FULL_MIN_G - LSM6DS0_ACCEL_TRUST_ZERO_MIN_G);
    }

    return (LSM6DS0_ACCEL_TRUST_ZERO_MAX_G - accel_g) /
           (LSM6DS0_ACCEL_TRUST_ZERO_MAX_G - LSM6DS0_ACCEL_TRUST_FULL_MAX_G);
}

static float lsm6ds0_rate_trust(float gyro_norm_dps)
{
    if (gyro_norm_dps <= LSM6DS0_GYRO_TRUST_FULL_MAX_DPS)
    {
        return 1.0f;
    }

    if (gyro_norm_dps >= LSM6DS0_GYRO_TRUST_ZERO_MIN_DPS)
    {
        return 0.0f;
    }

    return (LSM6DS0_GYRO_TRUST_ZERO_MIN_DPS - gyro_norm_dps) /
           (LSM6DS0_GYRO_TRUST_ZERO_MIN_DPS - LSM6DS0_GYRO_TRUST_FULL_MAX_DPS);
}

static void lsm6ds0_update_euler_from_quaternion(lsm6ds0_t *imu)
{
    float sinp;
    float roll_rad;
    float pitch_rad;

    roll_rad = atan2f(2.0f * ((imu->q0 * imu->q1) + (imu->q2 * imu->q3)),
                      1.0f - (2.0f * ((imu->q1 * imu->q1) + (imu->q2 * imu->q2))));
    sinp = 2.0f * ((imu->q0 * imu->q2) - (imu->q3 * imu->q1));
    sinp = lsm6ds0_clampf(sinp, -1.0f, 1.0f);
    pitch_rad = asinf(sinp);

    imu->roll_deg = roll_rad * LSM6DS0_RAD_TO_DEG;
    imu->pitch_deg = pitch_rad * LSM6DS0_RAD_TO_DEG;
}

static void lsm6ds0_set_quaternion_from_tilt(lsm6ds0_t *imu, float roll_rad, float pitch_rad)
{
    float half_roll = 0.5f * roll_rad;
    float half_pitch = 0.5f * pitch_rad;
    float sr = sinf(half_roll);
    float cr = cosf(half_roll);
    float sp = sinf(half_pitch);
    float cp = cosf(half_pitch);

    imu->q0 = cr * cp;
    imu->q1 = sr * cp;
    imu->q2 = cr * sp;
    imu->q3 = -sr * sp;

    lsm6ds0_update_euler_from_quaternion(imu);
}

static uint8_t lsm6ds0_initialize_tilt_from_accel(lsm6ds0_t *imu,
                                                   const lsm6ds0_measurements *sample)
{
    float ax = (float)sample->ax;
    float ay = (float)sample->ay;
    float az = (float)sample->az;
    float yz_norm = sqrtf((ay * ay) + (az * az));
    float pitch_rad;
    float roll_rad;

    if ((yz_norm < LSM6DS0_NORM_EPSILON) && (fabsf(ax) < LSM6DS0_NORM_EPSILON))
    {
        return 0U;
    }

    pitch_rad = -atan2f(ax, yz_norm);
    roll_rad = atan2f(ay, az);
    lsm6ds0_set_quaternion_from_tilt(imu, roll_rad, pitch_rad);
    imu->integral_fb_x = 0.0f;
    imu->integral_fb_y = 0.0f;
    imu->integral_fb_z = 0.0f;
    imu->initialized = 1U;

    return 1U;
}

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
    imu->q0 = 1.0f;
    imu->q1 = 0.0f;
    imu->q2 = 0.0f;
    imu->q3 = 0.0f;
    imu->integral_fb_x = 0.0f;
    imu->integral_fb_y = 0.0f;
    imu->integral_fb_z = 0.0f;
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

uint8_t lsm6ds0_is_known_whoami(uint8_t whoami)
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

    if (!lsm6ds0_write_reg(imu, LSM6DS0_REG_CTRL2_G, LSM6DS0_CTRL2_G_104HZ_500DPS))
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
    int32_t az_sum;
    float ax_avg;
    float ay_avg;
    float az_avg;
    float pitch_rad;
    float roll_rad;
    lsm6ds0_measurements sample;
    gx_sum = 0;
    gy_sum = 0;
    gz_sum = 0;
    ax_sum = 0;
    ay_sum = 0;
    az_sum = 0;
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
        az_sum += sample.az;

        lsm6ds0_delay_ms(sample_period_ms);
    }

    imu->gyro_x_bias_dps = ((float)gx_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;
    imu->gyro_y_bias_dps = ((float)gy_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;
    imu->gyro_z_bias_dps = ((float)gz_sum / (float)samples) * LSM6DS0_GYRO_DPS_PER_LSB;

    ax_avg = (float)ax_sum / (float)samples;
    ay_avg = (float)ay_sum / (float)samples;
    az_avg = (float)az_sum / (float)samples;
    pitch_rad = -atan2f(ax_avg, sqrtf((ay_avg * ay_avg) + (az_avg * az_avg)));
    roll_rad = atan2f(ay_avg, az_avg);
    lsm6ds0_set_quaternion_from_tilt(imu, roll_rad, pitch_rad);
    imu->integral_fb_x = 0.0f;
    imu->integral_fb_y = 0.0f;
    imu->integral_fb_z = 0.0f;
    imu->initialized = 1U;

    attitude_out->pitch_deg = imu->pitch_deg;
    attitude_out->roll_deg = imu->roll_deg;
    attitude_out->gx_dps = 0.0f;
    attitude_out->gy_dps = 0.0f;

    return 1U;
}

uint8_t lsm6ds0_update(lsm6ds0_t *imu, float dt_sec, lsm6ds0_attitude *attitude_out) {
    float gx_rps;
    float gy_rps;
    float gz_rps;
    float ax;
    float ay;
    float az;
    float accel_norm;
    float accel_weight;
    float vx;
    float vy;
    float vz;
    float ex;
    float ey;
    float ez;
    float q0;
    float q1;
    float q2;
    float q3;
    float inv_q_norm;
    float half_dt;
    float gx_dps;
    float gy_dps;
    float gz_dps;
    float gyro_norm_dps;
    lsm6ds0_measurements sample;

    if (dt_sec <= 0.0f)
    {
        return 0U;
    }

    if (!lsm6ds0_read_sample(imu, &sample)) {
        return 0U;
    }

    if (!imu->initialized)
    {
        if (!lsm6ds0_initialize_tilt_from_accel(imu, &sample))
        {
            return 0U;
        }
    }

    gx_dps = ((float)sample.gx * LSM6DS0_GYRO_DPS_PER_LSB) - imu->gyro_x_bias_dps;
    gy_dps = ((float)sample.gy * LSM6DS0_GYRO_DPS_PER_LSB) - imu->gyro_y_bias_dps;
    gz_dps = ((float)sample.gz * LSM6DS0_GYRO_DPS_PER_LSB) - imu->gyro_z_bias_dps;
    gx_rps = gx_dps * LSM6DS0_DEG_TO_RAD;
    gy_rps = gy_dps * LSM6DS0_DEG_TO_RAD;
    gz_rps = gz_dps * LSM6DS0_DEG_TO_RAD;
    gyro_norm_dps = sqrtf((gx_dps * gx_dps) + (gy_dps * gy_dps) + (gz_dps * gz_dps));

    ax = (float)sample.ax;
    ay = (float)sample.ay;
    az = (float)sample.az;
    accel_norm = sqrtf((ax * ax) + (ay * ay) + (az * az));

    if (accel_norm > LSM6DS0_NORM_EPSILON)
    {
        accel_weight = lsm6ds0_accel_trust(accel_norm);
        accel_weight *= lsm6ds0_rate_trust(gyro_norm_dps);

        ax /= accel_norm;
        ay /= accel_norm;
        az /= accel_norm;

        vx = 2.0f * ((imu->q1 * imu->q3) - (imu->q0 * imu->q2));
        vy = 2.0f * ((imu->q0 * imu->q1) + (imu->q2 * imu->q3));
        vz = (imu->q0 * imu->q0) - (imu->q1 * imu->q1) - (imu->q2 * imu->q2) + (imu->q3 * imu->q3);

        ex = ((ay * vz) - (az * vy)) * accel_weight;
        ey = ((az * vx) - (ax * vz)) * accel_weight;
        ez = ((ax * vy) - (ay * vx)) * accel_weight;

        imu->integral_fb_x += LSM6DS0_MAHONY_KI * ex * dt_sec;
        imu->integral_fb_y += LSM6DS0_MAHONY_KI * ey * dt_sec;
        imu->integral_fb_z += LSM6DS0_MAHONY_KI * ez * dt_sec;

        imu->integral_fb_x = lsm6ds0_clampf(imu->integral_fb_x, -LSM6DS0_INT_LIM_RADPS, LSM6DS0_INT_LIM_RADPS);
        imu->integral_fb_y = lsm6ds0_clampf(imu->integral_fb_y, -LSM6DS0_INT_LIM_RADPS, LSM6DS0_INT_LIM_RADPS);
        imu->integral_fb_z = lsm6ds0_clampf(imu->integral_fb_z, -LSM6DS0_INT_LIM_RADPS, LSM6DS0_INT_LIM_RADPS);

        gx_rps += (LSM6DS0_MAHONY_KP * ex) + imu->integral_fb_x;
        gy_rps += (LSM6DS0_MAHONY_KP * ey) + imu->integral_fb_y;
        gz_rps += (LSM6DS0_MAHONY_KP * ez) + imu->integral_fb_z;
    }

    q0 = imu->q0;
    q1 = imu->q1;
    q2 = imu->q2;
    q3 = imu->q3;
    half_dt = 0.5f * dt_sec;

    imu->q0 += ((-q1 * gx_rps) - (q2 * gy_rps) - (q3 * gz_rps)) * half_dt;
    imu->q1 += ((q0 * gx_rps) + (q2 * gz_rps) - (q3 * gy_rps)) * half_dt;
    imu->q2 += ((q0 * gy_rps) - (q1 * gz_rps) + (q3 * gx_rps)) * half_dt;
    imu->q3 += ((q0 * gz_rps) + (q1 * gy_rps) - (q2 * gx_rps)) * half_dt;

    inv_q_norm = sqrtf((imu->q0 * imu->q0) +
                       (imu->q1 * imu->q1) +
                       (imu->q2 * imu->q2) +
                       (imu->q3 * imu->q3));
    if (inv_q_norm <= LSM6DS0_NORM_EPSILON)
    {
        return 0U;
    }
    inv_q_norm = 1.0f / inv_q_norm;

    imu->q0 *= inv_q_norm;
    imu->q1 *= inv_q_norm;
    imu->q2 *= inv_q_norm;
    imu->q3 *= inv_q_norm;

    lsm6ds0_update_euler_from_quaternion(imu);

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
