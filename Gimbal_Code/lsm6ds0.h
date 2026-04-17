#ifndef LSM6DS0_H
#define LSM6DS0_H

#include <stdint.h>

static const uint8_t LSM6DS0_ADDR_LOW = 0x6AU;
static const uint8_t LSM6DS0_ADDR_HIGH = 0x6BU;

typedef struct
{
    uint8_t i2c_addr;
    float gyro_x_bias_dps;
    float gyro_y_bias_dps;
    float gyro_z_bias_dps;
    float pitch_deg;
    float roll_deg;
    uint8_t initialized;
} lsm6ds0_t;

typedef struct
{
    float pitch_deg;
    float roll_deg;
    float gx_dps;
    float gy_dps;
} lsm6ds0_attitude;

void lsm6ds0_create(lsm6ds0_t *imu);
uint8_t lsm6ds0_detect(lsm6ds0_t *imu, uint8_t *whoami);
uint8_t lsm6ds0_is_known_whoami(uint8_t whoami);
uint8_t lsm6ds0_init(lsm6ds0_t *imu);
uint8_t lsm6ds0_calibrate(lsm6ds0_t *imu,
                          uint16_t samples,
                          uint16_t sample_period_ms,
                          lsm6ds0_attitude *attitude_out);
uint8_t lsm6ds0_update(lsm6ds0_t *imu,
                       float dt_sec,
                       lsm6ds0_attitude *attitude_out);
uint8_t lsm6ds0_get_i2c_addr(const lsm6ds0_t *imu);
void lsm6ds0_get_gyro_bias_dps(const lsm6ds0_t *imu,
                               float *gx_bias_dps,
                               float *gy_bias_dps,
                               float *gz_bias_dps);

#endif /* LSM6DS0_H */
