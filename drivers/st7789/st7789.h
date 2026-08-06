/*
 * st7789.h - ST7789 SPI display driver (Raspberry Pi, user-space push)
 *
 * Intended for upper-layer integration (e.g. re3). The renderer only needs to
 * produce an RGB565 framebuffer and call st7789_flush* to push it to the panel.
 *
 * Backends: GPIO via /dev/gpiomem (root-free, see pi_gpio), SPI via spidev.
 * Performance: ~53 fps at 320x240 with the verified-stable 66.7MHz clock +
 * performance governor. 100MHz is faster (~78 fps) but glitches on this
 * panel/ribbon; 66.7MHz (core_freq/6) is rock-steady. See docs/06.
 *
 * Threading: an st7789_t handle is not thread-safe. If the render thread and
 * push thread are separate, serialize access to a single handle externally,
 * or use one handle per thread (sharing one SPI bus is not recommended).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ST7789_H
#define ST7789_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rotation, maps to MADCTL. 0=portrait 1=landscape 2=portrait flipped 3=landscape flipped. */
typedef enum {
    ST7789_ROTATE_0 = 0,
    ST7789_ROTATE_90 = 1,
    ST7789_ROTATE_180 = 2,
    ST7789_ROTATE_270 = 3
} st7789_rotation_t;

typedef struct {
    /* --- GPIO pins (BCM numbering) --- */
    int rst_pin; /* reset; <0 means not wired (hardware reset skipped) */
    int cs_pin;  /* chip select (driven by GPIO; SPI configured with SPI_NO_CS) */
    int dc_pin;  /* data/command select */
    int blk_pin; /* backlight; <0 means not controlled */

    /* --- SPI --- */
    const char* spi_dev; /* e.g. "/dev/spidev0.0" */
    uint32_t spi_hz;     /* SPI clock, e.g. 100000000 (actual rate quantized by core_freq divisor) */

    /* --- Geometry (effective resolution the caller renders at) --- */
    int width;                  /* effective width in pixels */
    int height;                 /* effective height in pixels */
    st7789_rotation_t rotation; /* rotation */
    int x_offset;               /* GRAM column offset (some 240x240 panels need it; default 0) */
    int y_offset;               /* GRAM row offset (default 0) */

    /* --- Transfer --- */
    uint32_t chunk_bytes; /* max bytes per SPI transfer (default 32768, avoids DMA-lite 32K limit) */
    int little_endian;    /* 1=send RGB565 little-endian (matches ENDIAN bit set in init), 0=big-endian */
    int invert;           /* 1=enable color inversion (0x21); most IPS ST7789 panels need this */
} st7789_config_t;

/*
 * Fill cfg with recommended defaults: 320x240 landscape, /dev/spidev0.0,
 * 80MHz request (-> 66.7MHz actual, the verified stable step; see docs/06),
 * pins RST=27 CS=8 DC=25 BLK=24, 32K chunk, little-endian, inverted.
 */
void st7789_config_default(st7789_config_t* cfg);

typedef struct st7789 st7789_t;

/*
 * Open and initialize the device. Returns a handle on success, NULL on failure
 * (errno may be set). Performs: gpiomem mapping, spidev configuration, hardware
 * reset, the ST7789 init command sequence, and a screen clear.
 */
st7789_t* st7789_open(const st7789_config_t* cfg);

/* Close and free. Safe to call with NULL. */
void st7789_close(st7789_t* dev);

/* Effective resolution (after rotation); the caller allocates the framebuffer accordingly. */
int st7789_get_width(const st7789_t* dev);
int st7789_get_height(const st7789_t* dev);

/*
 * Push a full frame. rgb565 points to width*height uint16_t values (row-major).
 * Returns 0 on success, <0 on failure.
 */
int st7789_flush(st7789_t* dev, const uint16_t* rgb565);

/*
 * Push a rectangular region. rgb565 holds w*h row-major pixels (contiguous).
 * A region outside the screen is rejected (returns <0). Use for dirty-rect /
 * differential updates.
 */
int st7789_flush_area(st7789_t* dev, int x, int y, int w, int h, const uint16_t* rgb565);

/* Fill the whole screen with a solid color. */
int st7789_fill(st7789_t* dev, uint16_t color);

/* Toggle backlight (requires blk_pin to be configured). */
void st7789_set_backlight(st7789_t* dev, int on);

/* RGB888 -> RGB565 helper for upper layers. */
static inline uint16_t st7789_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * Optional: best-effort tuning for optimal push performance -- set CPU governor
 * to performance and enlarge spidev bufsiz. Requires permission to write sysfs.
 * Returns 0 if all succeeded, <0 if partial (functionality unaffected, only perf).
 * Note: this is the key optimization found in this project -- the ondemand
 * governor downclocks the CPU during SPI DMA idle periods (see docs/06).
 */
int st7789_tune_system(uint32_t want_bufsiz);

#ifdef __cplusplus
}
#endif

#endif /* ST7789_H */
