/*
 * wiringPi.h - 最小 wiringPi 兼容 shim（仅供 st7789.c 端到端测试用）
 * GPIO 通过 /dev/gpiomem 直接 mmap 控制（BCM2835/2836/2837 通用）。
 * 只实现 st7789.c 用到的接口，不依赖系统 libwiringPi。
 */
#ifndef WIRINGPI_SHIM_H
#define WIRINGPI_SHIM_H

#include <stdint.h>

#define INPUT 0
#define OUTPUT 1
#define PUD_UP 2
#define PUD_DOWN 1
#define PUD_OFF 0

#define LOW 0
#define HIGH 1

int wiringPiSetupGpio(void);
void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);
void pullUpDnControl(int pin, int pud);
void delay(unsigned int ms);

/* 插桩：按调用类别拆解一帧刷屏的开销 */
void shim_profile_reset(void);
void shim_profile_report(int frames);

#endif
