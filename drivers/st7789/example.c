/*
 * example.c - ST7789 driver usage example / smoke test
 *
 * Shows the intended integration flow and measures full-frame fps.
 * Build: see CMakeLists.txt, or:
 *   gcc -O2 example.c st7789.c pi_gpio.c -o st7789_example
 *
 * SPDX-License-Identifier: MIT
 */
#include "st7789.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 200;

    st7789_config_t cfg;
    st7789_config_default(&cfg);
    /* Optionally override, e.g. cfg.spi_hz = 100000000; */

    /* Best-effort: performance governor for stable throughput. */
    st7789_tune_system(65536);

    st7789_t* dev = st7789_open(&cfg);
    if (!dev) {
        perror("st7789_open");
        return 1;
    }

    int w = st7789_get_width(dev);
    int h = st7789_get_height(dev);
    printf("panel %dx%d\n", w, h);

    uint16_t* fb = malloc((size_t)w * h * sizeof(uint16_t));
    if (!fb) {
        st7789_close(dev);
        return 1;
    }

    /* Alternate black/white full frames and measure fps. */
    int done = 0;
    double t0 = now_sec();
    for (int i = 0; i < frames; i++) {
        uint16_t c = (i & 1) ? 0xFFFF : 0x0000;
        for (int p = 0; p < w * h; p++)
            fb[p] = c;
        if (st7789_flush(dev, fb) < 0) {
            fprintf(stderr, "flush failed at frame %d\n", i);
            break;
        }
        done++;
    }
    double dt = now_sec() - t0;

    if (done > 0)
        printf("frames=%d  %.1f fps  %.3f ms/frame  %.2f MB/s\n",
            done, done / dt, dt / done * 1e3,
            (double)w * h * 2 * done / dt / (1024.0 * 1024.0));
    else
        fprintf(stderr, "no frames pushed successfully\n");

    free(fb);
    st7789_close(dev);
    return 0;
}
