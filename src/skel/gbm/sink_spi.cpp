/*
 * sink_spi.cpp - OutputSink pushing frames to an ST7789 SPI panel.
 *
 * Uses drivers/st7789. Nearest-neighbor scales the readback frame (any render
 * resolution) to the panel, flips to top-down, converts to RGB565. Color/clock
 * knobs are env-tunable so a panel can be dialed in without reflashing:
 *   RE3_SPI_INVERT, RE3_SPI_ENDIAN, RE3_SPI_ROT, RE3_SPI_HZ, RE3_SPI_BGR.
 * See docs/06.
 *
 * Active when RE3_OUTPUT_SPI is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_OUTPUT_SPI)

#include "output_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st7789.h"

static st7789_t	*sDev = 0;
static uint16_t	*sBuf = 0;		// panel-sized RGB565 scratch
static int		sW = 0, sH = 0;	// panel resolution
static int		sSwapRB = 0;

static bool
spi_init(int renderW, int renderH)
{
	(void)renderW; (void)renderH;

	st7789_config_t cfg;
	st7789_config_default(&cfg);

	const char *e;
	if ((e = getenv("RE3_SPI_INVERT")) != 0) cfg.invert = atoi(e);
	if ((e = getenv("RE3_SPI_ENDIAN")) != 0) cfg.little_endian = atoi(e);
	if ((e = getenv("RE3_SPI_ROT"))    != 0) cfg.rotation = (st7789_rotation_t)atoi(e);
	if ((e = getenv("RE3_SPI_HZ"))     != 0) cfg.spi_hz = (uint32_t)strtoul(e, 0, 10);
	if ((e = getenv("RE3_SPI_BGR"))    != 0) sSwapRB = atoi(e);

	st7789_tune_system(65536);	// performance governor: key perf knob (docs/06)

	sDev = st7789_open(&cfg);
	if (sDev == 0) {
		printf("spi: st7789_open failed; no output\n");
		return false;
	}
	sW = st7789_get_width(sDev);
	sH = st7789_get_height(sDev);
	sBuf = (uint16_t *)malloc((size_t)sW * sH * 2);
	printf("spi: ST7789 panel %dx%d\n", sW, sH);
	return sBuf != 0;
}

// Swap R and B channels of an RGB565 pixel (for BGR panels).
static inline uint16_t swap_rb_565(uint16_t p)
{
	uint16_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
	return (uint16_t)((b << 11) | (g << 5) | r);
}

static void
spi_present(const uint16_t *rgb565, int w, int h)
{
	if (sDev == 0 || sBuf == 0 || sW <= 0 || sH <= 0)
		return;

	// The frame is already RGB565 (display-native). Nearest-neighbor scale
	// render(w x h, bottom-up) -> panel(sW x sH, top-down). Fast path: when the
	// render size matches the panel and no channel swap, flip-copy whole rows.
	if (w == sW && h == sH && !sSwapRB) {
		for (int py = 0; py < sH; py++)
			memcpy(sBuf + py * sW, rgb565 + (h - 1 - py) * w, (size_t)sW * 2);
	} else {
		for (int py = 0; py < sH; py++) {
			int ry = (py * h) / sH;
			const uint16_t *srcRow = rgb565 + (h - 1 - ry) * w;	// flip
			uint16_t *dstRow = sBuf + py * sW;
			for (int px = 0; px < sW; px++) {
				uint16_t p = srcRow[(px * w) / sW];
				dstRow[px] = sSwapRB ? swap_rb_565(p) : p;
			}
		}
	}
	st7789_flush(sDev, sBuf);
}

static void
spi_terminate(void)
{
	if (sDev != 0) { st7789_close(sDev); sDev = 0; }
	if (sBuf != 0) { free(sBuf); sBuf = 0; }
}

static OutputSink sSink = { spi_init, spi_present, spi_terminate, "spi" };

OutputSink *OutputSink_Get(void) { return &sSink; }

#endif
