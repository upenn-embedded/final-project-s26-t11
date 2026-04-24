#define F_CPU 16000000UL

#include <avr/io.h>
#include <stdint.h>
#include <stdio.h>
#include <util/delay.h>

#include "uart.h"
#include "i2c.h"
#include "lsm6ds0.h"
#include <avr/interrupt.h>

//gimbaling boolean define
static volatile uint8_t gimbaling = 1U;

// delay between each loop iteration 
#define CONTROL_LOOP_MS 10U
static const float CONTROL_DT_SEC = 0.010f;

// num samples to take for IMU calibration
static const uint16_t CALIBRATION_SAMPLES = 200U;

// initial target angles in degrees
#define TARGET_PITCH_DEG_DEFAULT 0.0f
#define TARGET_ROLL_DEG_DEFAULT 0.0f

// PD control gains tuned experimentally
static const float PITCH_KP = 3.0f;
static const float ROLL_KP  = 3.0f;
static const float PITCH_KD = 0.0f;
static const float ROLL_KD = 0.0f;

// signs to flip motor direction
static const float PITCH_MOTOR_SIGN = 1.0f;
static const float ROLL_MOTOR_SIGN = 1.0f;

// servo pulse width limits and neutral position in microseconds
static const uint16_t PITCH_SERVO_MIN_US = 700U;
static const uint16_t PITCH_SERVO_MAX_US = 2300U;
static const uint16_t PITCH_SERVO_NEUTRAL_US = 1500U;

static const uint16_t ROLL_SERVO_MIN_US = 700U;
static const uint16_t ROLL_SERVO_MAX_US = 2300U;
static const uint16_t ROLL_SERVO_NEUTRAL_US = 1500U;

// conversion factor from pulse width is us to degrees
static const float SERVO_US_PER_DEG = 8.0f; // might need to adjust to 10.77

// trim values in microseconds to adjust for hardware imperfections
static const int16_t PITCH_SERVO_TRIM_US = 0;
static const int16_t ROLL_SERVO_TRIM_US = 0;

// servo timer configuration
static const uint16_t SERVO_FRAME_US = 20000U;
static const uint16_t SERVO_TIMER_TOP = 39999U;
static const uint16_t SERVO_TICKS_PER_US = 2U;

static float g_target_pitch_deg = TARGET_PITCH_DEG_DEFAULT;
static float g_target_roll_deg = TARGET_ROLL_DEG_DEFAULT;


ISR(INT0_vect) {
    gimbaling = !gimbaling;
}

static void print_thousandths(int32_t value)
{
    uint32_t mag;

    if (value < 0){
        putchar('-');
        mag = (uint32_t)(-value);
    } else {
        mag = (uint32_t)value;
    }

    printf("%lu.%03lu", (unsigned long)(mag/1000UL), (unsigned long)(mag%1000UL));
}

static uint16_t clamp_u16(int32_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < (int32_t)min_value) {
        return min_value;
    }

    if (value > (int32_t)max_value) {
        return max_value;
    }

    return (uint16_t)value;
}

static uint16_t servo_us_to_ticks(uint16_t pulse_us)
{
    return (uint16_t)(pulse_us * SERVO_TICKS_PER_US);
}

static void servo_init(void)
{
    DDRB |= (1U << DDB1) | (1U << DDB2);
    TCCR1A = (1U << COM1A1) | (1U << COM1B1) | (1U << WGM11);
    // Prescaler 8 gives 0.5us timer ticks at 16MHz so20ms frame with TOP=39999
    TCCR1B = (1U << WGM13) | (1U << WGM12) | (1U << CS11);
    ICR1 = SERVO_TIMER_TOP;
    OCR1A = servo_us_to_ticks(PITCH_SERVO_NEUTRAL_US);
    OCR1B = servo_us_to_ticks(ROLL_SERVO_NEUTRAL_US);
}

static void servo_set_us(uint16_t pitch_us, uint16_t roll_us)
{
    OCR1A = servo_us_to_ticks(pitch_us);
    OCR1B = servo_us_to_ticks(roll_us);
}

static void gimbal_control_step(const lsm6ds0_attitude *attitude, uint16_t *pitch_us, uint16_t *roll_us){
    float pitch_error;
    float roll_error;
    float pitch_cmd_deg;
    float roll_cmd_deg;
    int32_t pitch_us_cmd;
    int32_t roll_us_cmd;
    pitch_error = g_target_pitch_deg - attitude->pitch_deg;
    roll_error = g_target_roll_deg - attitude->roll_deg;
    pitch_cmd_deg = (PITCH_KP * pitch_error) - (PITCH_KD * attitude->gy_dps);
    roll_cmd_deg = (ROLL_KP * roll_error) - (ROLL_KD * attitude->gx_dps);
    pitch_us_cmd = (int32_t)PITCH_SERVO_NEUTRAL_US + PITCH_SERVO_TRIM_US + (int32_t)(PITCH_MOTOR_SIGN * pitch_cmd_deg * SERVO_US_PER_DEG);
    roll_us_cmd = (int32_t)ROLL_SERVO_NEUTRAL_US + ROLL_SERVO_TRIM_US + (int32_t)(ROLL_MOTOR_SIGN * roll_cmd_deg * SERVO_US_PER_DEG);
    *pitch_us = clamp_u16(pitch_us_cmd, PITCH_SERVO_MIN_US, PITCH_SERVO_MAX_US);
    *roll_us = clamp_u16(roll_us_cmd, ROLL_SERVO_MIN_US, ROLL_SERVO_MAX_US);
}

static void print_control_debug(const lsm6ds0_attitude *attitude, uint16_t pitch_us, uint16_t roll_us) {
    printf("pitch [degrees] = ");
    print_thousandths((int32_t)(attitude->pitch_deg * 1000.0f));
    printf("roll [degrees] = ");
    print_thousandths((int32_t)(attitude->roll_deg * 1000.0f));
    printf(" servo_us=(%u,%u)\r\n", pitch_us, roll_us);
}

void Initialize() {
    uart_init();
    i2c_init();
    servo_init();

    DDRD &= ~(1U << DDD2);
    PORTD |= (1U << PD2);

    EICRA |= (1U << ISC01);
    EICRA &= ~(1U << ISC00);
    EIMSK |= (1U << INT0);
    sei();
}

int main(void) {
    uint8_t whoami;
    uint8_t imu_ready;
    uint8_t debug_divider;
    lsm6ds0_t imu;
    lsm6ds0_attitude attitude;
    float gx_bias_dps;
    float gy_bias_dps;
    float gz_bias_dps;
    uint16_t pitch_us;
    uint16_t roll_us;

    Initialize();
    lsm6ds0_create(&imu);
    attitude.pitch_deg = 0.0f;
    attitude.roll_deg = 0.0f;
    attitude.gx_dps = 0.0f;
    attitude.gy_dps = 0.0f;
    pitch_us = PITCH_SERVO_NEUTRAL_US;
    roll_us = ROLL_SERVO_NEUTRAL_US;
    debug_divider = 0U;

    printf("\r\nATmega328PB 2-axis gimbal controller\r\n");
    printf("UART: 9600 baud, 8 data bits, 2 stop bits\r\n");
    printf("Servo outputs: OC1A=PB1 (pitch), OC1B=PB2 (roll)\r\n");
    printf("Servo frame: %u us (50Hz)\r\n", SERVO_FRAME_US);
    printf("Servo range us: pitch[%u,%u] roll[%u,%u]\r\n", PITCH_SERVO_MIN_US, PITCH_SERVO_MAX_US, ROLL_SERVO_MIN_US, ROLL_SERVO_MAX_US);
    imu_ready = 0U;

    while (1) {
        if (!imu_ready) {
            if (!lsm6ds0_detect(&imu, &whoami)) {
                printf("IMU not found at 0x%02X or 0x%02X Check  wiring.\r\n",
                       LSM6DS0_ADDR_LOW, LSM6DS0_ADDR_HIGH);
                _delay_ms(1000);
                continue;
            }
            printf("WHO_AM_I = 0x%02X at I2C addr 0x%02X\r\n", whoami,
                   lsm6ds0_get_i2c_addr(&imu));
            if (!lsm6ds0_is_known_whoami(whoami)) {
                printf("Warning: unexpected WHO_AM_I. Using LSM6 register map.\r\n");
            }
            if (!lsm6ds0_init(&imu)) {
                printf("IMU config write failed,retrying...\r\n");
                _delay_ms(1000);
                continue;
            }
            printf("Calibration: keep gimbal still for 2s...\r\n");
            if (!lsm6ds0_calibrate(&imu, CALIBRATION_SAMPLES, CONTROL_LOOP_MS, &attitude)) {
                printf("Calibration failed,retrying...\r\n");
                _delay_ms(500);
                continue;
            }
            g_target_pitch_deg = attitude.pitch_deg;
            g_target_roll_deg = attitude.roll_deg;
            lsm6ds0_get_gyro_bias_dps(&imu, &gx_bias_dps, &gy_bias_dps, &gz_bias_dps);
            printf("Cal done: target pitch=");
            print_thousandths((int32_t)(g_target_pitch_deg * 1000.0f));
            printf(" roll=");
            print_thousandths((int32_t)(g_target_roll_deg * 1000.0f));
            printf(" gyro_bias[dps]=(");
            print_thousandths((int32_t)(gx_bias_dps * 1000.0f));
            printf(",");
            print_thousandths((int32_t)(gy_bias_dps * 1000.0f));
            printf(",");
            print_thousandths((int32_t)(gz_bias_dps * 1000.0f));
            printf(")\r\n");
            printf("Stabilizing around startup pitch/roll. Yaw ignored.\r\n");
            imu_ready = 1U;
        }
        if (!lsm6ds0_update(&imu, CONTROL_DT_SEC, &attitude)) {
            printf("IMU read failed, reinitializing...\r\n");
            servo_set_us(PITCH_SERVO_NEUTRAL_US, ROLL_SERVO_NEUTRAL_US);
            lsm6ds0_create(&imu);
            imu_ready = 0U;
            _delay_ms(200);
            continue;
        }
        if(gimbaling){
            gimbal_control_step(&attitude, &pitch_us, &roll_us);
            servo_set_us(pitch_us, roll_us);
        }
        debug_divider++;
        if (debug_divider >= 10U) {
            debug_divider = 0U;
            print_control_debug(&attitude, pitch_us, roll_us);
        }
        _delay_ms(CONTROL_LOOP_MS);
    }
}