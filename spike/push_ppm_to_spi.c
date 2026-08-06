// Push a P6 PPM image to the ST7789 SPI panel via st7789_flush.
// Reads an RGB888 PPM, converts to RGB565, sends to panel.
// Use to verify SPI color mapping without running re3.
//
// Build: arm-linux-gnueabihf-gcc --sysroot=$SR -O2 spike/push_ppm_to_spi.c \
//   drivers/st7789/st7789.c drivers/st7789/pi_gpio.c -Idrivers/st7789 \
//   -o push_ppm_to_spi -lgbm -lEGL -lGLESv2
// Run:   ./push_ppm_to_spi frame.ppm [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "st7789.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: push_ppm_to_spi <file.ppm> [seconds]\n"); return 1; }
    int secs = argc > 2 ? atoi(argv[2]) : 10;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    int W, H, maxv;
    if (fscanf(f, "P6 %d %d %d ", &W, &H, &maxv) != 3) { fprintf(stderr, "bad PPM\n"); return 1; }
    uint8_t *rgb = malloc(W * H * 3);
    if (!fread(rgb, 3, W * H, f)) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    printf("PPM: %dx%d maxval=%d\n", W, H, maxv);

    // Convert RGB888 -> RGB565 (little-endian, matches driver default)
    uint16_t *fb = malloc(W * H * 2);
    for (int i = 0; i < W * H; i++) {
        uint8_t r = rgb[i*3], g = rgb[i*3+1], b = rgb[i*3+2];
        fb[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    free(rgb);

    st7789_config_t cfg;
    st7789_config_default(&cfg); // 80MHz request -> 66.7MHz actual, little_endian=1
    // Allow env override for testing
    const char *e;
    if ((e = getenv("RE3_SPI_HZ")))     cfg.spi_hz = (uint32_t)strtoul(e, 0, 10);
    if ((e = getenv("RE3_SPI_ENDIAN"))) cfg.little_endian = atoi(e);
    if ((e = getenv("RE3_SPI_INVERT"))) cfg.invert = atoi(e);
    printf("SPI hz=%u endian=%d invert=%d\n", cfg.spi_hz, cfg.little_endian, cfg.invert);

    st7789_tune_system(65536);
    st7789_t *dev = st7789_open(&cfg);
    if (!dev) { perror("st7789_open"); return 1; }
    printf("panel %dx%d, showing for %d seconds...\n", st7789_get_width(dev), st7789_get_height(dev), secs);

    uint16_t *panel_buf = malloc(st7789_get_width(dev) * st7789_get_height(dev) * 2);
    // Nearest-neighbor scale W*H -> panel, no flip (PPM already top-down)
    int pw = st7789_get_width(dev), ph = st7789_get_height(dev);
    for (int py = 0; py < ph; py++) {
        int ry = (py * H) / ph;
        for (int px = 0; px < pw; px++)
            panel_buf[py * pw + px] = fb[(ry * W) + (px * W) / pw];
    }
    free(fb);

    for (int t = 0; t < secs * 10; t++) {
        st7789_flush(dev, panel_buf);
        usleep(100000);
    }
    free(panel_buf);
    st7789_close(dev);
    printf("done\n");
    return 0;
}
