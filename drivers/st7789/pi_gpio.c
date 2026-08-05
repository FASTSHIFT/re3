/*
 * pi_gpio.c - Raspberry Pi fast GPIO library
 *
 * Based on FASTSHIFT/pi_gpio (MIT, Copyright (c) 2022-2024 _VIFEXTech)
 *   https://github.com/FASTSHIFT/pi_gpio
 *
 * Changes:
 *   1. Use /dev/gpiomem instead of /dev/mem: root-free (user just needs to be
 *      in the gpio group), and the mapping starts at the GPIO block so there is
 *      no need to know each Pi generation's peripheral base (the original
 *      hardcoded 0x20000000 only fits the first-gen Pi/Zero1; Zero 2 W is
 *      0x3f000000).
 *   2. set_mode writes the GPFSEL register directly, dropping the wiringPi dep.
 *   3. Keep the original library's core: set/clr values go through the
 *      GPSET/GPCLR registers with a single direct write (fastest path).
 *
 * SPDX-License-Identifier: MIT
 */
#include "pi_gpio.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define BLOCK_SIZE (4 * 1024)

static volatile uint32_t* gpio = NULL;

/* GPIO register word offsets (relative to the GPIO block base) */
#define GPIO_REG_SET (gpio + 7)  /* GPSET0 @ 0x1C */
#define GPIO_REG_CLR (gpio + 10) /* GPCLR0 @ 0x28 */
#define GPIO_REG_LEV (gpio + 13) /* GPLEV0 @ 0x34 */

int pi_gpio_init(void)
{
    if (gpio)
        return 0; /* idempotent */

    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("pi_gpio: open /dev/gpiomem");
        return -1;
    }

    void* map = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (map == MAP_FAILED) {
        perror("pi_gpio: mmap gpiomem");
        return -1;
    }

    gpio = (volatile uint32_t*)map;
    return 0;
}

void pi_gpio_set_mode(uint8_t bcm_pin, int mode)
{
    if (!gpio || bcm_pin > 53)
        return;
    int reg = bcm_pin / 10; /* GPFSELn */
    int shift = (bcm_pin % 10) * 3;
    uint32_t v = gpio[reg];
    v &= ~(0x7u << shift);
    if (mode == PI_GPIO_OUTPUT)
        v |= (0x1u << shift); /* 001=output; INPUT=000 */
    gpio[reg] = v;
}

void pi_gpio_set_value(uint8_t bcm_pin, int value)
{
    if (!gpio || bcm_pin > 53)
        return;
    value ? (*GPIO_REG_SET = (1u << bcm_pin)) : (*GPIO_REG_CLR = (1u << bcm_pin));
}

int pi_gpio_get_value(uint8_t bcm_pin)
{
    if (!gpio || bcm_pin > 53)
        return 0;
    return (*GPIO_REG_LEV >> bcm_pin) & 0x1u;
}
