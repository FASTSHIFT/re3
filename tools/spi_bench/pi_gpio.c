/*
 * pi_gpio.c - Raspberry Pi 快速 GPIO 库
 *
 * 基于 FASTSHIFT/pi_gpio (MIT, Copyright (c) 2022-2024 _VIFEXTech)
 *   https://github.com/FASTSHIFT/pi_gpio
 *
 * 本测试版改造点：
 *   1. 用 /dev/gpiomem 替代 /dev/mem：免 root（pi 用户在 gpio 组即可），
 *      且 gpiomem 映射起点即 GPIO 块，无需关心各代 Pi 的 peri base
 *      （原库硬编码 0x20000000 仅适用于初代 Pi/Zero1；Zero 2 W 是 0x3f000000）。
 *   2. set_mode 用 GPFSEL 寄存器直写，去除对 wiringPi 的依赖。
 *   3. 保留原库精髓：set/clr 值走 GPSET/GPCLR 寄存器直接单写，最快路径。
 */
#include "pi_gpio.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define BLOCK_SIZE (4 * 1024)

static volatile uint32_t *gpio = NULL;

/* GPIO 寄存器字偏移（相对 GPIO 块起始）*/
#define GPIO_REG_SET (gpio + 7)  /* GPSET0 @ 0x1C */
#define GPIO_REG_CLR (gpio + 10) /* GPCLR0 @ 0x28 */
#define GPIO_REG_LEV (gpio + 13) /* GPLEV0 @ 0x34 */

int
pi_gpio_init(void)
{
	int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
	if(fd < 0) {
		perror("open /dev/gpiomem");
		return -1;
	}

	void *map = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if(map == MAP_FAILED) {
		perror("mmap gpiomem");
		return -1;
	}

	gpio = (volatile uint32_t *)map;
	return 0;
}

void
pi_gpio_set_mode(uint8_t bcm_pin, int mode)
{
	if(!gpio) return;
	int reg = bcm_pin / 10; /* GPFSELn */
	int shift = (bcm_pin % 10) * 3;
	uint32_t v = gpio[reg];
	v &= ~(0x7u << shift);
	if(mode == PI_GPIO_OUTPUT) v |= (0x1u << shift); /* 001 = output; INPUT=000 */
	gpio[reg] = v;
}

void
pi_gpio_set_value(uint8_t bcm_pin, int value)
{
	if(!gpio) return;
	value ? (*GPIO_REG_SET = (1u << bcm_pin)) : (*GPIO_REG_CLR = (1u << bcm_pin));
}

int
pi_gpio_get_value(uint8_t bcm_pin)
{
	if(!gpio) return 0;
	return (*GPIO_REG_LEV >> bcm_pin) & 0x1u;
}
