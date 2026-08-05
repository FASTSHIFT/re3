/*
 * wiringPiSPI.h - 最小 wiringPiSPI 兼容 shim（spidev 后端）
 * 只实现 st7789.c 用到的接口。
 */
#ifndef WIRINGPISPI_SHIM_H
#define WIRINGPISPI_SHIM_H

#include <stdint.h>

int wiringPiSPISetupMode(int channel, int speed, int mode);
int wiringPiSPIDataRW(int channel, unsigned char* data, int len);

#endif
