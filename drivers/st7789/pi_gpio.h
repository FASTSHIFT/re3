/*
 * pi_gpio.h - Raspberry Pi fast GPIO library
 *
 * Based on FASTSHIFT/pi_gpio (MIT, Copyright (c) 2022-2024 _VIFEXTech)
 *   https://github.com/FASTSHIFT/pi_gpio
 * Changes: use /dev/gpiomem (root-free) instead of /dev/mem; set_mode writes
 * the GPFSEL register directly to drop the wiringPi dependency.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PI_GPIO_H
#define PI_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PI_GPIO_INPUT 0
#define PI_GPIO_OUTPUT 1

/* Initialize: mmap /dev/gpiomem. Returns 0 on success, <0 on failure. Idempotent. */
int pi_gpio_init(void);

void pi_gpio_set_mode(uint8_t bcm_pin, int mode);
void pi_gpio_set_value(uint8_t bcm_pin, int value);
int pi_gpio_get_value(uint8_t bcm_pin);

#ifdef __cplusplus
}
#endif

#endif /* PI_GPIO_H */
