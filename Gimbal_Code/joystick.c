#include <avr/io.h>
#include <stdint.h>

#define JOY_CENTER 330
#define JOY_DEADBAND 150
#define JOY_UP_THRESHOLD (JOY_CENTER + JOY_DEADBAND)
#define JOY_DOWN_THRESHOLD (JOY_CENTER - JOY_DEADBAND)

#define EXT_UP_DDR      DDRC
#define EXT_UP_PORT     PORTC
#define EXT_UP_PIN_REG  PINC
#define EXT_UP_PIN      PC1

#define EXT_DOWN_DDR      DDRC
#define EXT_DOWN_PORT     PORTC
#define EXT_DOWN_PIN_REG  PINC
#define EXT_DOWN_PIN      PC2

void joystick_init(void)
{
    EXT_UP_DDR   &= ~(1 << EXT_UP_PIN);
    EXT_DOWN_DDR &= ~(1 << EXT_DOWN_PIN);

    EXT_UP_PORT   &= ~(1 << EXT_UP_PIN);
    EXT_DOWN_PORT &= ~(1 << EXT_DOWN_PIN);

    DDRC  &= ~(1 << DDC0);
    PORTC &= ~(1 << PORTC0);

    ADMUX  = (1 << REFS0);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

uint16_t joystick_read_raw(void)
{
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {
    }
    return ADC;
}

int8_t joystick_read_vertical_step(void)
{
    uint8_t up_high   = (EXT_UP_PIN_REG   & (1 << EXT_UP_PIN))   != 0;
    uint8_t down_high = (EXT_DOWN_PIN_REG & (1 << EXT_DOWN_PIN)) != 0;
    uint16_t joystick_value = joystick_read_raw();

    if (up_high && !down_high) {
        return -1;
    }
    if (down_high && !up_high) {
        return 1;
    }
    if (joystick_value > JOY_UP_THRESHOLD) {
        return -1;
    }
    if (joystick_value < JOY_DOWN_THRESHOLD) {
        return 1;
    }
    return 0;
}
