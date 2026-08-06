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

static void
spi_present(const uint8_t *rgba, int w, int h)
{
	if (sDev == 0 || sBuf == 0 || sW <= 0 || sH <= 0)
		return;

	// Nearest-neighbor scale render(w x h, bottom-up) -> panel(sW x sH,
	// top-down), RGBA8 -> RGB565. sSwapRB handles BGR panels.
	for (int py = 0; py < sH; py++) {
		int ry = (py * h) / sH;
		const uint8_t *srcRow = rgba + (h - 1 - ry) * w * 4;	// flip
		uint16_t *dstRow = sBuf + py * sW;
		for (int px = 0; px < sW; px++) {
			int rx = (px * w) / sW;
			const uint8_t *s = srcRow + rx * 4;
			uint8_t r = s[0], g = s[1], b = s[2];
			if (sSwapRB) { uint8_t t = r; r = b; b = t; }
			dstRow[px] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
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
