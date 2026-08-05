/*
 * spi_profile.c - 拆解 SPI 推送链路开销
 *
 * 目的：定位 60fps (需 9.0 MB/s @320x240) 达不到的开销来源。
 * 方法：测量单次 spidev 传输在不同 size 下的耗时，线性回归
 *          t(size) = a + b*size
 *   a = 每次传输固定开销 (ioctl syscall + DMA setup + CS/inter-word gap)
 *   b = 每字节线传输时间   (1/b = 实际有效线速率)
 *
 * 再据此预测：一帧 150KB 拆成 N 块时 t_frame = N*a + 150K*b，
 * 反推要 60fps 需要什么条件。
 *
 * 用法: [SPI_HZ=..] spi_profile [device]
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

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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

    long bufsiz = -1;
    FILE* f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    if (f) { if (fscanf(f, "%ld", &bufsiz) != 1) bufsiz = -1; fclose(f); }

    size_t maxsz = 65536;
    uint8_t* buf = malloc(maxsz);
    for (size_t i = 0; i < maxsz; i++) buf[i] = (uint8_t)i;

    printf("=== SPI link profiler ===\n");
    printf("device=%s requested_hz=%u actual_hz=%u bufsiz=%ld\n", dev, hz, rd, bufsiz);
    printf("clock bit-time  : %.4f ns/byte (8/%u)\n", 8.0 / rd * 1e9, rd);
    printf("clock byte-rate : %.3f MB/s (theoretical @actual_hz)\n\n", rd / 8.0 / (1024.0*1024.0));

    /* 测不同 size 的单次传输平均耗时 */
    size_t sizes[] = { 256, 1024, 4096, 8192, 16384, 32768, 49152, 65535 };
    int nsz = sizeof(sizes)/sizeof(sizes[0]);
    double xs[16], ys[16];
    int np = 0;

    printf("%8s | %10s | %10s | %8s\n", "size", "us/xfer", "MB/s", "reps");
    for (int i = 0; i < nsz; i++) {
        size_t s = sizes[i];
        if (bufsiz > 0 && s > (size_t)bufsiz) continue;
        int reps = (int)(20000000UL / (s + 1000)); /* 大块少测，小块多测 */
        if (reps < 50) reps = 50;
        if (reps > 5000) reps = 5000;

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)buf;
        tr.len = s; tr.speed_hz = hz; tr.bits_per_word = 8;

        /* 预热 */
        ioctl(fd, SPI_IOC_MESSAGE(1), &tr);

        double t0 = now_sec();
        for (int r = 0; r < reps; r++)
            ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
        double dt = now_sec() - t0;

        double us = dt / reps * 1e6;
        double mbps = (double)s * reps / dt / (1024.0*1024.0);
        printf("%8zu | %10.2f | %10.2f | %8d\n", s, us, mbps, reps);

        xs[np] = (double)s; ys[np] = dt / reps; np++;
    }

    /* 线性回归 t = a + b*size */
    double sx=0, sy=0, sxx=0, sxy=0;
    for (int i = 0; i < np; i++) { sx+=xs[i]; sy+=ys[i]; sxx+=xs[i]*xs[i]; sxy+=xs[i]*ys[i]; }
    double b = (np*sxy - sx*sy) / (np*sxx - sx*sx);
    double a = (sy - b*sx) / np;

    printf("\n=== linear fit  t(size) = a + b*size ===\n");
    printf("a (fixed/xfer)  : %.2f us  (ioctl + DMA setup + CS/gap)\n", a*1e6);
    printf("b (per-byte)    : %.4f ns/byte\n", b*1e9);
    printf("effective line  : %.3f MB/s  (1/b)\n", 1.0/b / (1024.0*1024.0));
    printf("line efficiency : %.1f%%  (vs clock %.3f MB/s)\n",
        (1.0/b) / (rd/8.0) * 100.0, rd/8.0/(1024.0*1024.0));

    /* 预测一帧 150KB 在不同 chunk 下的 fps */
    printf("\n=== predicted 320x240 (150KB) frame @ this clock ===\n");
    double frame = 320.0*240.0*2.0;
    size_t chunks[] = { 4096, 32768, 65535 };
    for (int i = 0; i < 3; i++) {
        double c = (double)chunks[i];
        double n = (frame + c - 1) / c; /* ceil */
        double t = n*a + frame*b;
        printf("  chunk=%6zu: %.0f xfers, %.3f ms/frame, %.1f fps\n",
            chunks[i], n, t*1e3, 1.0/t);
    }
    /* 达到 60fps 需要的单帧预算 */
    double t60 = 1.0/60.0;
    double line_only = frame*b;
    printf("\n60fps budget    : %.3f ms/frame\n", t60*1e3);
    printf("line time alone : %.3f ms  (irreducible @this clock)\n", line_only*1e3);
    if (line_only < t60)
        printf("=> 线速率够，剩 %.3f ms 给固定开销，最多容 %.0f 次 xfer\n",
            (t60-line_only)*1e3, (t60-line_only)/a);
    else
        printf("=> 线速率就不够！必须提高时钟或减少像素/字节数\n");

    free(buf); close(fd);
    return 0;
}
