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

/* --- Pull-up/down ---
 * Two hardware generations:
 *   - BCM2835/6/7 (Pi 0/1/2/3): GPPUD @0x94 + GPPUDCLK0 @0x98 clocking sequence.
 *   - BCM2711 (Pi 4): per-pin GPIO_PUP_PDN_CNTRL_REG0..3 @0xE4.., 2 bits/pin,
 *     encoding 00=off 01=up 10=down (note: opposite bit meaning vs legacy).
 * We detect the Pi 4 by probing whether the PUP_PDN registers are non-reserved.
 * Simpler and robust: write BOTH sequences; the one that doesn't exist on a
 * given SoC is a harmless write within the mapped 4K GPIO block.
 */
#define GPIO_REG_PUD (gpio + 37)      /* GPPUD @ 0x94 (word 37) */
#define GPIO_REG_PUDCLK0 (gpio + 38)  /* GPPUDCLK0 @ 0x98 (word 38) */
#define GPIO_REG_PUPPDN0 (gpio + 57)  /* GPIO_PUP_PDN_CNTRL_REG0 @ 0xE4 (word 57) */

static void short_wait(void)
{
    for (volatile int i = 0; i < 200; i++)
        ; /* ~150 cycles, satisfies the legacy 150-cycle setup/hold */
}

void pi_gpio_set_pull(uint8_t bcm_pin, int pull)
{
    if (!gpio || bcm_pin > 53)
        return;

    /* BCM2711 (Pi 4) path: 2 bits per pin, 16 pins per 32-bit reg.
     * encoding: 00=none, 01=pull-up, 10=pull-down */
    uint32_t pv2711 = (pull == PI_GPIO_PULL_UP) ? 1u : (pull == PI_GPIO_PULL_DOWN) ? 2u : 0u;
    volatile uint32_t* reg = GPIO_REG_PUPPDN0 + (bcm_pin / 16);
    int shift = (bcm_pin % 16) * 2;
    uint32_t v = *reg;
    v &= ~(0x3u << shift);
    v |= (pv2711 << shift);
    *reg = v;

    /* BCM2835/6/7 (Pi 0/1/2/3) legacy path.
     * encoding: 0=off, 1=pull-down, 2=pull-up */
    uint32_t pv283x = (pull == PI_GPIO_PULL_UP) ? 2u : (pull == PI_GPIO_PULL_DOWN) ? 1u : 0u;
    *GPIO_REG_PUD = pv283x;
    short_wait();
    *(GPIO_REG_PUDCLK0 + (bcm_pin / 32)) = (1u << (bcm_pin % 32));
    short_wait();
    *GPIO_REG_PUD = 0;
    *(GPIO_REG_PUDCLK0 + (bcm_pin / 32)) = 0;
}
