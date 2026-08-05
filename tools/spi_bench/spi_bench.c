/*
 * spi_bench.c - ST7789 / spidev 吞吐基准测试
 *
 * 目标：量化 docs/06-ST7789-SPI推送优化分析.md 中描述的瓶颈：
 *   - spidev bufsiz 默认 4096 的软限制
 *   - 不同传输块大小 (chunk) 对吞吐的影响
 *   - polling vs DMA 路径 (>=96 字节走 DMA)
 *
 * 纯 spidev ioctl 实现，不依赖 wiringPi。
 * 只测 SPI 数据传输吞吐 (MOSI 方向)，不做 ST7789 初始化 / 不驱动 DC-RST。
 * 因此可安全测量，即使没接屏也能跑 (数据丢进总线)。
 *
 * 用法:
 *   spi_bench [device] [spi_hz] [frame_bytes] [iterations]
 *   默认: /dev/spidev0.0  60000000  (320*240*2=153600)  100
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

/* 单次 SPI 写传输 (半双工写)，最多 len 字节 */
static int spi_write_chunk(int fd, const uint8_t* buf, size_t len, uint32_t hz)
{
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)buf;
    tr.rx_buf = 0;
    tr.len = len;
    tr.speed_hz = hz;
    tr.bits_per_word = 8;
    tr.delay_usecs = 0;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

/* 推送一帧 frame_bytes，按 chunk 大小拆分 */
static int push_frame(int fd, const uint8_t* frame, size_t frame_bytes,
    size_t chunk, uint32_t hz, int* out_ioctls)
{
    size_t off = 0;
    int ioctls = 0;
    while (off < frame_bytes) {
        size_t n = frame_bytes - off;
        if (n > chunk)
            n = chunk;
        if (spi_write_chunk(fd, frame + off, n, hz) < 1) {
            perror("SPI_IOC_MESSAGE");
            return -1;
        }
        ioctls++;
        off += n;
    }
    if (out_ioctls)
        *out_ioctls = ioctls;
    return 0;
}

static void bench(int fd, const uint8_t* frame, size_t frame_bytes,
    size_t chunk, uint32_t hz, int iters)
{
    int ioctls_per_frame = 0;
    /* 预热 */
    push_frame(fd, frame, frame_bytes, chunk, hz, &ioctls_per_frame);

    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        if (push_frame(fd, frame, frame_bytes, chunk, hz, NULL) < 0)
            return;
    }
    double dt = now_sec() - t0;

    double total_bytes = (double)frame_bytes * iters;
    double mbps = total_bytes / dt / (1024.0 * 1024.0);
    double fps = iters / dt;

    printf("  chunk=%6zu B | ioctls/frame=%3d | %6.2f MB/s | %6.1f fps | %.3f ms/frame\n",
        chunk, ioctls_per_frame, mbps, fps, dt / iters * 1000.0);
}

int main(int argc, char** argv)
{
    const char* dev = (argc > 1) ? argv[1] : "/dev/spidev0.0";
    uint32_t hz = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 60000000u;
    size_t frame_bytes = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : (320u * 240u * 2u);
    int iters = (argc > 4) ? atoi(argv[4]) : 100;

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open spidev");
        return 1;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0)
        perror("SPI_IOC_WR_MODE");
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
        perror("SPI_IOC_WR_BITS_PER_WORD");
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0)
        perror("SPI_IOC_WR_MAX_SPEED_HZ");

    uint32_t rd_hz = 0;
    ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &rd_hz);

    /* 读取内核 spidev bufsiz */
    long bufsiz = -1;
    FILE* f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    if (f) {
        if (fscanf(f, "%ld", &bufsiz) != 1)
            bufsiz = -1;
        fclose(f);
    }

    uint8_t* frame = malloc(frame_bytes);
    if (!frame) {
        fprintf(stderr, "malloc %zu failed\n", frame_bytes);
        return 1;
    }
    /* 填充渐变图案，避免全 0 */
    for (size_t i = 0; i < frame_bytes; i++)
        frame[i] = (uint8_t)(i & 0xFF);

    printf("=== spidev SPI throughput benchmark ===\n");
    printf("device       : %s\n", dev);
    printf("requested hz : %u\n", hz);
    printf("actual hz    : %u\n", rd_hz);
    printf("frame bytes  : %zu (%.1f KB)\n", frame_bytes, frame_bytes / 1024.0);
    printf("iterations   : %d\n", iters);
    printf("kernel bufsiz: %ld\n", bufsiz);
    printf("theoretical  : %.2f MB/s (hz/8)\n", rd_hz / 8.0 / (1024.0 * 1024.0));
    printf("\n");

    /* 测试多种 chunk 大小 */
    size_t chunks[] = { 4096, 8192, 16384, 32768, 65535 };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        /* chunk 超过 frame 时跳过重复 */
        if (i > 0 && chunks[i] >= frame_bytes && chunks[i - 1] >= frame_bytes)
            break;
        bench(fd, frame, frame_bytes, chunks[i], hz, iters);
    }

    free(frame);
    close(fd);
    return 0;
}
