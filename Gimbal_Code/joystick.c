#include <avr/io.h>
#include <stdint.h>

#define JOY_CENTER 330
#define JOY_DEADBAND 150
#define JOY_UP_THRESHOLD (JOY_CENTER + JOY_DEADBAND)
#define JOY_DOWN_THRESHOLD (JOY_CENTER - JOY_DEADBAND)

void joystick_init(void)
{
    DDRC  &= ~((1 << DDC0) | (1 << DDC1));
    PORTC &= ~((1 << PORTC0) | (1 << PORTC1));

    ADMUX  = (1 << REFS0);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

uint16_t joystick_read_raw(void)
{
    ADMUX = (1 << REFS0);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
    return ADC;
}

uint16_t joystick_read_horizontal_raw(void)
{
    ADMUX = (1 << REFS0) | (1 << MUX0);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
    return ADC;
}

int8_t joystick_read_vertical_step(void)
{
    uint16_t value = joystick_read_raw();
    if (value > JOY_UP_THRESHOLD)   return -1;
    if (value < JOY_DOWN_THRESHOLD) return  1;
    return 0;
}

int8_t joystick_read_horizontal_step(void)
{
    uint16_t value = joystick_read_horizontal_raw();
    if (value > JOY_UP_THRESHOLD)   return  1;
    if (value < JOY_DOWN_THRESHOLD) return -1;
    return 0;
}
