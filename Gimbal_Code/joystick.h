#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <avr/io.h>
#include <stdint.h>

void    joystick_init(void);
uint16_t joystick_read_raw(void);
int8_t  joystick_read_vertical_step(void);

#endif
