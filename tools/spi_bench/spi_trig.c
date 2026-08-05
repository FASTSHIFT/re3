/* spi_trig.c - 固定 size 触发 N 次 SPI 传输，供内核 tracepoint 抓取 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    const char* dev = argc > 1 ? argv[1] : "/dev/spidev0.0";
    uint32_t hz = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 100000000u;
    uint32_t size = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 10) : 32768u;
    int reps = argc > 4 ? atoi(argv[4]) : 20;

    int fd = open(dev, O_RDWR);
    if (fd < 0) return 1;
    uint8_t mode = SPI_MODE_0, bits = 8;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz);

    uint8_t* buf = malloc(size);
    memset(buf, 0xA5, size);
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)buf;
    tr.len = size; tr.speed_hz = hz; tr.bits_per_word = 8;

    for (int i = 0; i < reps; i++)
        ioctl(fd, SPI_IOC_MESSAGE(1), &tr);

    free(buf); close(fd);
    return 0;
}
