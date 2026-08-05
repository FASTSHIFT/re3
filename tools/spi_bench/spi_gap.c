/*
 * spi_gap.c - 测 bcm2835 SPI 每字节时钟周期，对比 polling(<96B) vs DMA(>=96B) 路径
 *
 * bcm2835 驱动：<96 字节走 CPU polling（理论无字节间隙），>=96 走 DMA。
 * 若 polling 路径 clk/byte 接近 8，而 DMA 路径 ~13，则证明间隙来自 DMA 引擎。
 *
 * 用法: [SPI_HZ=..] spi_gap [device]
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char** argv)
{
    const char* dev = (argc > 1) ? argv[1] : "/dev/spidev0.0";
    const char* env_hz = getenv("SPI_HZ");
    uint32_t hz = env_hz ? (uint32_t)strtoul(env_hz, NULL, 10) : 100000000u;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    uint8_t mode = SPI_MODE_0, bits = 8;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz);
    uint32_t rd = 0; ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd);

    uint8_t* buf = malloc(65536);
    for (int i = 0; i < 65536; i++) buf[i] = i;

    printf("=== SPI per-byte clock probe ===  actual_hz=%u\n", rd);
    printf("ideal: 8.00 clk/byte (%.2f ns/byte)\n\n", 8.0/rd*1e9);
    printf("%8s | %6s | %10s | %10s | %8s | %s\n",
        "size", "path", "ns/xfer", "ns/byte", "clk/byte", "eff%");

    /* 覆盖 polling(<96) 与 DMA(>=96) 边界 */
    size_t sizes[] = { 32, 64, 88, 95, 96, 128, 256, 512, 4096, 65535 };
    int n = sizeof(sizes)/sizeof(sizes[0]);
    for (int i = 0; i < n; i++) {
        size_t s = sizes[i];
        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)buf; tr.len = s;
        tr.speed_hz = hz; tr.bits_per_word = 8;

        int reps = (int)(30000000UL / (s + 500));
        if (reps < 200) reps = 200; if (reps > 20000) reps = 20000;

        ioctl(fd, SPI_IOC_MESSAGE(1), &tr); /* warm */
        double t0 = now_ns();
        for (int r = 0; r < reps; r++) ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
        double per = (now_ns() - t0) / reps;

        double ns_byte = per / s;
        double clk_byte = ns_byte / (1e9/rd);
        const char* path = (s < 96) ? "poll" : "dma";
        printf("%8zu | %6s | %10.1f | %10.3f | %8.2f | %.1f%%\n",
            s, path, per, ns_byte, clk_byte, 8.0/clk_byte*100.0);
    }
    /* 注：小传输 per-xfer 固定开销会污染 ns/byte，重点看大 size 的 clk/byte 渐近值，
     * 及 polling 小块在扣除固定开销后的斜率。 */
    free(buf); close(fd);
    return 0;
}
