/*
 * wiringPi_shim.c - 最小 wiringPi / wiringPiSPI 实现
 *
 * GPIO: 直接 mmap /dev/gpiomem 操作 BCM2835 GPIO 寄存器（Pi Zero 2 W = BCM2837）。
 * SPI : 通过 /dev/spidevN.0 (spidev ioctl)。
 *
 * 只实现 ref_lv_gba_emu/port/rpi/st7789.c 用到的接口，使其无需系统 libwiringPi
 * 即可端到端驱动 ST7789。
 *
 * 频率可用环境变量 SPI_HZ 覆盖 st7789.c 里硬编码的 60MHz（便于扫频而不改驱动）。
 * spidev 设备可用 SPI_DEV 覆盖，默认 /dev/spidev0.0。
 */
#include "wiringPi.h"
#include "wiringPiSPI.h"
#include "pi_gpio.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ---------- 插桩计数器（SHIM_PROFILE=1 时启用） ---------- */
typedef struct {
    unsigned long gpio_calls;
    double gpio_ns;
    unsigned long spi_small_calls; /* len<=64 的小传输（cmd/addr window） */
    double spi_small_ns;
    unsigned long spi_big_calls; /* 大数据块传输 */
    double spi_big_ns;
    unsigned long spi_big_bytes;
} shim_stats_t;

static shim_stats_t g_st;
static int g_prof = 0;

static inline double prof_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

void shim_profile_reset(void)
{
    memset(&g_st, 0, sizeof(g_st));
    g_prof = 1;
}

void shim_profile_report(int frames)
{
    if (!g_prof)
        return;
    double tot = g_st.gpio_ns + g_st.spi_small_ns + g_st.spi_big_ns;
    printf("\n=== shim call breakdown (%d frames) ===\n", frames);
    printf("%-16s %10s %12s %10s %12s\n", "class", "calls", "total_ms", "per_call_us", "per_frame_us");
    printf("%-16s %10lu %12.3f %10.3f %12.3f\n", "GPIO(digWrite)",
        g_st.gpio_calls, g_st.gpio_ns / 1e6, g_st.gpio_ns / g_st.gpio_calls / 1e3,
        g_st.gpio_ns / frames / 1e3);
    printf("%-16s %10lu %12.3f %10.3f %12.3f\n", "SPI small(<=64)",
        g_st.spi_small_calls, g_st.spi_small_ns / 1e6,
        g_st.spi_small_calls ? g_st.spi_small_ns / g_st.spi_small_calls / 1e3 : 0,
        g_st.spi_small_ns / frames / 1e3);
    printf("%-16s %10lu %12.3f %10.3f %12.3f\n", "SPI big(data)",
        g_st.spi_big_calls, g_st.spi_big_ns / 1e6,
        g_st.spi_big_calls ? g_st.spi_big_ns / g_st.spi_big_calls / 1e3 : 0,
        g_st.spi_big_ns / frames / 1e3);
    printf("%-16s %10s %12.3f %10s %12.3f\n", "TOTAL", "", tot / 1e6, "", tot / frames / 1e3);
    if (g_st.spi_big_bytes) {
        double big_line = g_st.spi_big_bytes / (g_st.spi_big_ns / 1e9) / (1024.0 * 1024.0);
        printf("big-data effective line: %.3f MB/s (bytes=%lu)\n", big_line, g_st.spi_big_bytes);
    }
    printf("share: GPIO %.1f%% | small-ioctl %.1f%% | big-data %.1f%%\n",
        g_st.gpio_ns / tot * 100, g_st.spi_small_ns / tot * 100, g_st.spi_big_ns / tot * 100);
}

/* ---------- GPIO：转发到 pi_gpio 快速库（FASTSHIFT/pi_gpio） ---------- */

int wiringPiSetupGpio(void)
{
    return pi_gpio_init();
}

void pinMode(int pin, int mode)
{
    pi_gpio_set_mode((uint8_t)pin, mode == OUTPUT ? PI_GPIO_OUTPUT : PI_GPIO_INPUT);
}

void digitalWrite(int pin, int value)
{
    if (g_prof) {
        double t0 = prof_now_ns();
        pi_gpio_set_value((uint8_t)pin, value);
        g_st.gpio_ns += prof_now_ns() - t0;
        g_st.gpio_calls++;
        return;
    }
    pi_gpio_set_value((uint8_t)pin, value);
}

int digitalRead(int pin)
{
    return pi_gpio_get_value((uint8_t)pin);
}

void pullUpDnControl(int pin, int pud)
{
    (void)pin;
    (void)pud;
    /* 测试用不到按键上拉，留空 */
}

void delay(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------- SPI via spidev ---------- */

static int g_spi_fd = -1;
static uint32_t g_spi_hz = 60000000u;

int wiringPiSPISetupMode(int channel, int speed, int mode)
{
    const char* dev = getenv("SPI_DEV");
    char path[64];
    if (dev) {
        snprintf(path, sizeof(path), "%s", dev);
    } else {
        snprintf(path, sizeof(path), "/dev/spidev0.%d", channel);
    }

    g_spi_fd = open(path, O_RDWR);
    if (g_spi_fd < 0) {
        perror("open spidev");
        return -1;
    }

    /* 允许用 SPI_HZ 覆盖驱动硬编码频率 */
    const char* env_hz = getenv("SPI_HZ");
    g_spi_hz = env_hz ? (uint32_t)strtoul(env_hz, NULL, 10) : (uint32_t)speed;

    /* 手动控制 CS（st7789.c 用 GPIO 控 CS），让 spidev 不碰硬件 CE */
    uint8_t m = (uint8_t)mode | SPI_NO_CS;
    uint8_t bits = 8;
    if (ioctl(g_spi_fd, SPI_IOC_WR_MODE, &m) < 0)
        perror("SPI_IOC_WR_MODE");
    if (ioctl(g_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
        perror("SPI_IOC_WR_BITS_PER_WORD");
    if (ioctl(g_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &g_spi_hz) < 0)
        perror("SPI_IOC_WR_MAX_SPEED_HZ");

    uint32_t rd = 0;
    ioctl(g_spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd);
    fprintf(stderr, "[shim] spidev=%s requested_hz=%u actual_hz=%u\n", path, g_spi_hz, rd);
    return g_spi_fd;
}

int wiringPiSPIDataRW(int channel, unsigned char* data, int len)
{
    (void)channel;
    if (g_spi_fd < 0)
        return -1;
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)data;
    tr.rx_buf = 0; /* st7789.c 不读回，半双工写 */
    tr.len = (uint32_t)len;
    tr.speed_hz = g_spi_hz;
    tr.bits_per_word = 8;

    if (g_prof) {
        double t0 = prof_now_ns();
        int r = ioctl(g_spi_fd, SPI_IOC_MESSAGE(1), &tr);
        double dt = prof_now_ns() - t0;
        if (len <= 64) {
            g_st.spi_small_ns += dt;
            g_st.spi_small_calls++;
        } else {
            g_st.spi_big_ns += dt;
            g_st.spi_big_calls++;
            g_st.spi_big_bytes += (unsigned long)len;
        }
        return r;
    }
    return ioctl(g_spi_fd, SPI_IOC_MESSAGE(1), &tr);
}
