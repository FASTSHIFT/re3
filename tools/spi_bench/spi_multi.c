/*
 * spi_multi.c - 验证"一次 ioctl 提交多个 transfer"能否消除 per-call 开销
 *
 * 对比三种推整帧(150KB)方式：
 *   A) N 次 ioctl，每次 1 个 transfer（当前参考驱动做法）
 *   B) 1 次 ioctl，SPI_IOC_MESSAGE(N) 批量提交 N 个 transfer
 *   C) 1 次 ioctl，单个大 transfer（若 bufsiz 允许）
 *
 * 内核 tracepoint 已证明单次 DMA 传输效率 ~97%，瓶颈在 syscall/调度往返。
 * 批量提交应能逼近线速率。
 *
 * 用法: [SPI_HZ=..] spi_multi [frame_bytes] [chunk] [iters]
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
    const char* env_hz = getenv("SPI_HZ");
    uint32_t hz = env_hz ? (uint32_t)strtoul(env_hz, NULL, 10) : 100000000u;
    size_t frame = argc > 1 ? strtoul(argv[1], 0, 10) : 320u * 240u * 2u;
    size_t chunk = argc > 2 ? strtoul(argv[2], 0, 10) : 32768u;
    int iters = argc > 3 ? atoi(argv[3]) : 200;

    int fd = open("/dev/spidev0.0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    uint8_t mode = SPI_MODE_0, bits = 8;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz);
    uint32_t rd = 0; ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd);

    long bufsiz = 4096;
    FILE* f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    if (f) { if (fscanf(f, "%ld", &bufsiz) != 1) bufsiz = 4096; fclose(f); }

    uint8_t* buf = malloc(frame);
    for (size_t i = 0; i < frame; i++) buf[i] = i;

    size_t nchunk = (frame + chunk - 1) / chunk;

    printf("=== SPI multi-transfer test ===\n");
    printf("actual_hz=%u frame=%zu chunk=%zu nchunk=%zu bufsiz=%ld iters=%d\n",
        rd, frame, chunk, nchunk, bufsiz, iters);
    printf("theoretical @actual_hz: %.2f MB/s\n\n", rd/8.0/(1024*1024));

    /* ---- A) N 次 ioctl ---- */
    {
        double t0 = now_sec();
        for (int it = 0; it < iters; it++) {
            size_t off = 0;
            while (off < frame) {
                size_t n = frame - off; if (n > chunk) n = chunk;
                struct spi_ioc_transfer tr;
                memset(&tr, 0, sizeof(tr));
                tr.tx_buf = (unsigned long)(buf + off);
                tr.len = n; tr.speed_hz = hz; tr.bits_per_word = 8;
                ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
                off += n;
            }
        }
        double dt = now_sec() - t0;
        printf("A) %zu x ioctl(1):     %6.2f MB/s | %6.1f fps | %.3f ms/frame\n",
            nchunk, frame*iters/dt/(1024*1024), iters/dt, dt/iters*1e3);
    }

    /* ---- B) 1 次 ioctl 批量 N transfer ---- */
    if (nchunk <= 512) {
        struct spi_ioc_transfer* trs = calloc(nchunk, sizeof(*trs));
        double t0 = now_sec();
        for (int it = 0; it < iters; it++) {
            size_t off = 0;
            for (size_t k = 0; k < nchunk; k++) {
                size_t n = frame - off; if (n > chunk) n = chunk;
                trs[k].tx_buf = (unsigned long)(buf + off);
                trs[k].len = n; trs[k].speed_hz = hz; trs[k].bits_per_word = 8;
                off += n;
            }
            ioctl(fd, SPI_IOC_MESSAGE(nchunk), trs);
        }
        double dt = now_sec() - t0;
        printf("B) 1 x ioctl(%zu):     %6.2f MB/s | %6.1f fps | %.3f ms/frame\n",
            nchunk, frame*iters/dt/(1024*1024), iters/dt, dt/iters*1e3);
        free(trs);
    }

    /* ---- C) 单个大 transfer（需 bufsiz>=frame） ---- */
    if ((long)frame <= bufsiz) {
        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)buf;
        tr.len = frame; tr.speed_hz = hz; tr.bits_per_word = 8;
        double t0 = now_sec();
        for (int it = 0; it < iters; it++)
            ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
        double dt = now_sec() - t0;
        printf("C) 1 x ioctl(1 big): %6.2f MB/s | %6.1f fps | %.3f ms/frame\n",
            frame*iters/dt/(1024*1024), iters/dt, dt/iters*1e3);
    } else {
        printf("C) skipped (frame %zu > bufsiz %ld)\n", frame, bufsiz);
    }

    free(buf); close(fd);
    return 0;
}
