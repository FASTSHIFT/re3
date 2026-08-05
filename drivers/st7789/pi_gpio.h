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

/* Pull resistor selection for pi_gpio_set_pull */
#define PI_GPIO_PULL_OFF 0
#define PI_GPIO_PULL_DOWN 1
#define PI_GPIO_PULL_UP 2

/* Initialize: mmap /dev/gpiomem. Returns 0 on success, <0 on failure. Idempotent. */
int pi_gpio_init(void);

void pi_gpio_set_mode(uint8_t bcm_pin, int mode);
void pi_gpio_set_value(uint8_t bcm_pin, int value);
int pi_gpio_get_value(uint8_t bcm_pin);

/*
 * Enable/disable the internal pull-up/down on a pin. Supports both the
 * BCM2835/6/7 legacy pull sequence (GPPUD/GPPUDCLK) and the BCM2711 (Pi 4)
 * GPIO_PUP_PDN_CNTRL registers, auto-detected. Needed for buttons wired to
 * ground (use PI_GPIO_PULL_UP so an unpressed pin reads 1, pressed reads 0).
 */
void pi_gpio_set_pull(uint8_t bcm_pin, int pull);

#ifdef __cplusplus
}
#endif

#endif /* PI_GPIO_H */
