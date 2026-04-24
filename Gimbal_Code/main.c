#define F_CPU 16000000UL

#include <avr/io.h>
#include <stdint.h>
#include <stdio.h>
#include <util/delay.h>

#include "uart.h"
#include "i2c.h"
#include "lsm6ds0.h"
#include "joystick.h"
#include <avr/interrupt.h>

#define JOYSTICK_STEP_DEG 0.5f

//gimbaling boolean define
static volatile uint8_t gimbaling = 1U;

// delay between each loop iteration 
#define CONTROL_LOOP_MS 10U
static const float CONTROL_DT_SEC = 0.010f;
static const uint8_t DEBUG_PRINT_DIVIDER = 25U;

// control mode selector (change this to switch behavior)
static const uint8_t CONTROL_MODE_NORMAL_PID = 0U;
static const uint8_t CONTROL_MODE_NEGATIVE_IMU_TEST = 1U;
static volatile uint8_t g_control_mode = 0U;

// num samples to take for IMU calibration
static const uint16_t CALIBRATION_SAMPLES = 200U;

// initial target angles in degrees
#define TARGET_PITCH_DEG_DEFAULT 0.0f
#define TARGET_ROLL_DEG_DEFAULT 0.0f

// PD control gains tuned experimentally
// static const float PITCH_KP = 3.5f;
// static const float ROLL_KP  = 3.5f;
static const float PITCH_KP = 0.8f;
static const float ROLL_KP  = 0.8f;

// static const float PITCH_KI = 0.005f;
// static const float ROLL_KI = 0.05f;

static const float PITCH_KI = 0.5f;
static const float ROLL_KI = 0.5f;
// static const float PITCH_KI = 0.0f;
// static const float ROLL_KI = 0.0f;


// static const float PITCH_KD = 0.05f;
// static const float ROLL_KD = 0.005f;
static const float PITCH_KD = 0.0015f;
static const float ROLL_KD = 0.0015f;

// integral term limits in deg*s to prevent windup
static const float PITCH_INT_LIM_DEG_S = 40.0f;
static const float ROLL_INT_LIM_DEG_S = 40.0f;

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
// static const float SERVO_US_PER_DEG = 8.0f; // might need to adjust to 10.77
// static const float SERVO_US_PER_DEG = 7.0f; // might need to adjust to 10.77
// static const float SERVO_US_PER_DEG = 10.77f; // might need to adjust to 10.77
// static const float SERVO_US_PER_DEG = 5.56f; // might need to adjust to 10.77
static const float SERVO_US_PER_DEG = 33.0f; // might need to adjust to 10.77

// trim values in microseconds to adjust for hardware imperfections
static const int16_t PITCH_SERVO_TRIM_US = 0;
static const int16_t ROLL_SERVO_TRIM_US = 0;

// servo timer configuration
static const uint16_t SERVO_FRAME_US = 20000U;
static const uint16_t SERVO_TIMER_TOP = 39999U;
static const uint16_t SERVO_TICKS_PER_US = 2U;

static float g_target_pitch_deg = TARGET_PITCH_DEG_DEFAULT;
static float g_target_roll_deg = TARGET_ROLL_DEG_DEFAULT;
static float g_pitch_error_integral_deg_s = 0.0f;
static float g_roll_error_integral_deg_s = 0.0f;

typedef struct
{
    float pitch_p_deg;
    float pitch_i_deg;
    float pitch_d_deg;
    float pitch_response_deg;
    float roll_p_deg;
    float roll_i_deg;
    float roll_d_deg;
    float roll_response_deg;
} gimbal_control_debug;

static volatile uint8_t isr_count = 0U;

ISR(PCINT2_vect) {
    isr_count++;
    gimbaling = (PIND & (1U << PD2)) ? 1U : 0U;
    if (gimbaling) {
        PORTD |=  (1U << PD3);
    } else {
        PORTD &= ~(1U << PD3);
    }
    printf("ISR fired! count=%u gimbaling=%u\r\n", isr_count, gimbaling);
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

static float clamp_f32(float value, float min_value, float max_value)
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

static void control_reset_integrators(void)
{
    g_pitch_error_integral_deg_s = 0.0f;
    g_roll_error_integral_deg_s = 0.0f;
}

static void gimbal_control_step(const lsm6ds0_attitude *attitude,
                                uint16_t *pitch_us,
                                uint16_t *roll_us,
                                gimbal_control_debug *debug){
    float pitch_error;
    float roll_error;
    float pitch_int_candidate;
    float roll_int_candidate;
    float pitch_cmd_deg;
    float roll_cmd_deg;
    float pitch_cmd_deg_unsat;
    float roll_cmd_deg_unsat;
    uint8_t pitch_integrator_blocked;
    uint8_t roll_integrator_blocked;
    int32_t pitch_us_cmd_unsat;
    int32_t roll_us_cmd_unsat;
    int32_t pitch_us_cmd;
    int32_t roll_us_cmd;

    pitch_error = g_target_pitch_deg - attitude->pitch_deg;
    roll_error = g_target_roll_deg - attitude->roll_deg;

    pitch_int_candidate = clamp_f32(g_pitch_error_integral_deg_s + (pitch_error * CONTROL_DT_SEC),
                                    -PITCH_INT_LIM_DEG_S,
                                    PITCH_INT_LIM_DEG_S);
    roll_int_candidate = clamp_f32(g_roll_error_integral_deg_s + (roll_error * CONTROL_DT_SEC),
                                   -ROLL_INT_LIM_DEG_S,
                                   ROLL_INT_LIM_DEG_S);

    pitch_cmd_deg_unsat = (PITCH_KP * pitch_error) + (PITCH_KI * pitch_int_candidate) - (PITCH_KD * attitude->gy_dps);
    roll_cmd_deg_unsat = (ROLL_KP * roll_error) + (ROLL_KI * roll_int_candidate) - (ROLL_KD * attitude->gx_dps);

    pitch_us_cmd_unsat = (int32_t)PITCH_SERVO_NEUTRAL_US + PITCH_SERVO_TRIM_US +
                         (int32_t)(PITCH_MOTOR_SIGN * pitch_cmd_deg_unsat * SERVO_US_PER_DEG);
    roll_us_cmd_unsat = (int32_t)ROLL_SERVO_NEUTRAL_US + ROLL_SERVO_TRIM_US +
                        (int32_t)(ROLL_MOTOR_SIGN * roll_cmd_deg_unsat * SERVO_US_PER_DEG);

    pitch_integrator_blocked = (uint8_t)(((pitch_us_cmd_unsat > (int32_t)PITCH_SERVO_MAX_US) && (pitch_error > 0.0f)) ||
                                         ((pitch_us_cmd_unsat < (int32_t)PITCH_SERVO_MIN_US) && (pitch_error < 0.0f)));
    roll_integrator_blocked = (uint8_t)(((roll_us_cmd_unsat > (int32_t)ROLL_SERVO_MAX_US) && (roll_error > 0.0f)) ||
                                        ((roll_us_cmd_unsat < (int32_t)ROLL_SERVO_MIN_US) && (roll_error < 0.0f)));

    if (!pitch_integrator_blocked)
    {
        g_pitch_error_integral_deg_s = pitch_int_candidate;
        pitch_cmd_deg = pitch_cmd_deg_unsat;
    }
    else
    {
        pitch_cmd_deg = (PITCH_KP * pitch_error) + (PITCH_KI * g_pitch_error_integral_deg_s) - (PITCH_KD * attitude->gy_dps);
    }

    if (!roll_integrator_blocked)
    {
        g_roll_error_integral_deg_s = roll_int_candidate;
        roll_cmd_deg = roll_cmd_deg_unsat;
    }
    else
    {
        roll_cmd_deg = (ROLL_KP * roll_error) + (ROLL_KI * g_roll_error_integral_deg_s) - (ROLL_KD * attitude->gx_dps);
    }

    debug->pitch_p_deg = PITCH_KP * pitch_error;
    debug->pitch_i_deg = PITCH_KI * g_pitch_error_integral_deg_s;
    debug->pitch_d_deg = -PITCH_KD * attitude->gy_dps;
    debug->pitch_response_deg = pitch_cmd_deg;

    debug->roll_p_deg = ROLL_KP * roll_error;
    debug->roll_i_deg = ROLL_KI * g_roll_error_integral_deg_s;
    debug->roll_d_deg = -ROLL_KD * attitude->gx_dps;
    debug->roll_response_deg = roll_cmd_deg;

    pitch_us_cmd = (int32_t)PITCH_SERVO_NEUTRAL_US + PITCH_SERVO_TRIM_US + (int32_t)(PITCH_MOTOR_SIGN * pitch_cmd_deg * SERVO_US_PER_DEG);
    roll_us_cmd = (int32_t)ROLL_SERVO_NEUTRAL_US + ROLL_SERVO_TRIM_US + (int32_t)(ROLL_MOTOR_SIGN * roll_cmd_deg * SERVO_US_PER_DEG);
    *pitch_us = clamp_u16(pitch_us_cmd, PITCH_SERVO_MIN_US, PITCH_SERVO_MAX_US);
    *roll_us = clamp_u16(roll_us_cmd, ROLL_SERVO_MIN_US, ROLL_SERVO_MAX_US);
}

static void gimbal_negative_imu_test_step(const lsm6ds0_attitude *attitude,
                                          uint16_t *pitch_us,
                                          uint16_t *roll_us,
                                          gimbal_control_debug *debug)
{
    float pitch_cmd_deg = -attitude->pitch_deg;
    float roll_cmd_deg = -attitude->roll_deg;
    int32_t pitch_us_cmd;
    int32_t roll_us_cmd;

    // In test mode, report only the final response term. P/I/D terms are not used.
    debug->pitch_p_deg = 0.0f;
    debug->pitch_i_deg = 0.0f;
    debug->pitch_d_deg = 0.0f;
    debug->pitch_response_deg = pitch_cmd_deg;

    debug->roll_p_deg = 0.0f;
    debug->roll_i_deg = 0.0f;
    debug->roll_d_deg = 0.0f;
    debug->roll_response_deg = roll_cmd_deg;

    pitch_us_cmd = (int32_t)PITCH_SERVO_NEUTRAL_US + PITCH_SERVO_TRIM_US +
                   (int32_t)(PITCH_MOTOR_SIGN * pitch_cmd_deg * SERVO_US_PER_DEG);
    roll_us_cmd = (int32_t)ROLL_SERVO_NEUTRAL_US + ROLL_SERVO_TRIM_US +
                  (int32_t)(ROLL_MOTOR_SIGN * roll_cmd_deg * SERVO_US_PER_DEG);

    *pitch_us = clamp_u16(pitch_us_cmd, PITCH_SERVO_MIN_US, PITCH_SERVO_MAX_US);
    *roll_us = clamp_u16(roll_us_cmd, ROLL_SERVO_MIN_US, ROLL_SERVO_MAX_US);
}

static void print_control_debug(const lsm6ds0_attitude *attitude,
                                const gimbal_control_debug *debug,
                                uint16_t pitch_us,
                                uint16_t roll_us) {
    printf("ang=(");
    print_thousandths((int32_t)(attitude->pitch_deg * 1000.0f));
    printf(",");
    print_thousandths((int32_t)(attitude->roll_deg * 1000.0f));
    printf(") p=(");
    print_thousandths((int32_t)(debug->pitch_p_deg * 1000.0f));
    printf(",");
    print_thousandths((int32_t)(debug->roll_p_deg * 1000.0f));
    printf(") i=(");
    print_thousandths((int32_t)(debug->pitch_i_deg * 1000.0f));
    printf(",");
    print_thousandths((int32_t)(debug->roll_i_deg * 1000.0f));
    printf(") d=(");
    print_thousandths((int32_t)(debug->pitch_d_deg * 1000.0f));
    printf(",");
    print_thousandths((int32_t)(debug->roll_d_deg * 1000.0f));
    printf(") resp=(");
    print_thousandths((int32_t)(debug->pitch_response_deg * 1000.0f));
    printf(",");
    print_thousandths((int32_t)(debug->roll_response_deg * 1000.0f));
    printf(") servo_us=(%u,%u)\r\n", pitch_us, roll_us);
}

void Initialize() {
    uart_init();
    i2c_init();
    servo_init();
    joystick_init();
    control_reset_integrators();

    DDRD &= ~(1U << DDD2);
    PORTD |= (1U << PD2);

    DDRD  |=  (1U << DDD3);
    PORTD &= ~(1U << PD3);

    PCICR  |= (1U << PCIE2);
    PCMSK2 |= (1U << PCINT18);
    sei();
}

int main(void) {
    uint8_t whoami;
    uint8_t active_control_mode;
    uint8_t imu_ready;
    uint8_t debug_divider;
    lsm6ds0_t imu;
    lsm6ds0_attitude attitude;
    gimbal_control_debug control_debug;
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
    active_control_mode = 0xFFU;
    debug_divider = 0U;

    control_debug.pitch_p_deg = 0.0f;
    control_debug.pitch_i_deg = 0.0f;
    control_debug.pitch_d_deg = 0.0f;
    control_debug.pitch_response_deg = 0.0f;
    control_debug.roll_p_deg = 0.0f;
    control_debug.roll_i_deg = 0.0f;
    control_debug.roll_d_deg = 0.0f;
    control_debug.roll_response_deg = 0.0f;

    printf("\r\nATmega328PB 2-axis gimbal controller\r\n");
    printf("UART: 9600 baud, 8 data bits, 2 stop bits\r\n");
    printf("Servo outputs: OC1A=PB1 (pitch), OC1B=PB2 (roll)\r\n");
    printf("Servo frame: %u us (50Hz)\r\n", SERVO_FRAME_US);
    printf("Servo range us: pitch[%u,%u] roll[%u,%u]\r\n", PITCH_SERVO_MIN_US, PITCH_SERVO_MAX_US, ROLL_SERVO_MIN_US, ROLL_SERVO_MAX_US);
        printf("Control mode: %s\r\n",
            (g_control_mode == CONTROL_MODE_NEGATIVE_IMU_TEST) ? "NEGATIVE_IMU_TEST" : "NORMAL_PID");
    imu_ready = 0U;

    while (1) {
        if (gimbaling) {
            PORTD |=  (1U << PD3);
        } else {
            PORTD &= ~(1U << PD3);
        }

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
            control_reset_integrators();
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
            control_reset_integrators();
            lsm6ds0_create(&imu);
            imu_ready = 0U;
            _delay_ms(200);
            continue;
        }
        {
            int8_t joy_v = joystick_read_vertical_step();
            if (joy_v != 0) {
                g_target_pitch_deg += (float)joy_v * JOYSTICK_STEP_DEG;
            }
        }

        if(gimbaling){

        if (active_control_mode != g_control_mode)
        {
            active_control_mode = g_control_mode;
            control_reset_integrators();
            printf("Control mode changed: %s\r\n",
                   (active_control_mode == CONTROL_MODE_NEGATIVE_IMU_TEST) ? "NEGATIVE_IMU_TEST" : "NORMAL_PID");
        }

        if (active_control_mode == CONTROL_MODE_NEGATIVE_IMU_TEST)
        {
            gimbal_negative_imu_test_step(&attitude, &pitch_us, &roll_us, &control_debug);
        }
        else
        {
            gimbal_control_step(&attitude, &pitch_us, &roll_us, &control_debug);
        }

            servo_set_us(pitch_us, roll_us);
        }
        debug_divider++;
        if (debug_divider >= DEBUG_PRINT_DIVIDER) {
            debug_divider = 0U;
            print_control_debug(&attitude, &control_debug, pitch_us, roll_us);
            printf("joy_v=%u step=%d target=(",
                   joystick_read_raw(), (int)joystick_read_vertical_step());
            print_thousandths((int32_t)(g_target_pitch_deg * 1000.0f));
            printf(",");
            print_thousandths((int32_t)(g_target_roll_deg * 1000.0f));
            printf(")\r\n");
        }
        _delay_ms(CONTROL_LOOP_MS);
    }
}