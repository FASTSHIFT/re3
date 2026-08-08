/*
 * pi_gpio.h - Raspberry Pi 快速 GPIO 库
 * 基于 FASTSHIFT/pi_gpio (MIT, Copyright (c) 2022-2024 _VIFEXTech)，
 * 改造：用 /dev/gpiomem(免 root) 替代 /dev/mem，set_mode 用寄存器直写去除 wiringPi 依赖。
 */
#ifndef __PI_GPIO_H
#define __PI_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PI_GPIO_INPUT 0
#define PI_GPIO_OUTPUT 1

int
pi_gpio_init(void);
void
pi_gpio_set_mode(uint8_t bcm_pin, int mode);
void
pi_gpio_set_value(uint8_t bcm_pin, int value);
int
pi_gpio_get_value(uint8_t bcm_pin);

#ifdef __cplusplus
}
#endif

#endif
