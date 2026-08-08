/*
 * pattern.c - ST7789 diagnostic pattern for finding the safe SPI clock.
 *
 * Renders a static test image designed to expose signal-integrity glitches
 * (color noise / tearing / shifted pixels) that appear when the SPI clock is
 * too high for the panel + ribbon:
 *   - vertical color bars (R/G/B/white/black) — corruption shows as wrong colors
 *   - 1px grid overlay                          — broken/smeared lines = glitch
 *   - horizontal gradient band                  — banding/jumps = bit errors
 *
 * Push the same image once and hold, so you can inspect it calmly. Re-run at
 * different clocks to find the highest stable one.
 *
 * Usage:  ./st7789_pattern <spi_hz> [hold_seconds]
 *   hold_seconds > 0 : push once, hold static image that many seconds
 *   hold_seconds = 0 : push once, hold until Ctrl-C
 *   hold_seconds < 0 : CONTINUOUSLY re-push the same image at full rate until
 *                      Ctrl-C. Use this to detect instability: a marginal clock
 *                      produces random bit errors, so the image visibly
 *                      shimmers/flickers; a stable clock stays rock-steady.
 *   e.g.  ./st7789_pattern 100000000 -1     # 100MHz, continuous refresh
 *         ./st7789_pattern 62500000 5       # ~50MHz effective, hold 5s
 *
 * SPDX-License-Identifier: MIT
 */
#include "st7789.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int g_stop = 0;
static void
on_sig(int s)
{
	(void)s;
	g_stop = 1;
}

int
main(int argc, char **argv)
{
	if(argc < 2) {
		fprintf(stderr, "usage: %s <spi_hz> [hold_seconds]\n", argv[0]);
		return 2;
	}
	uint32_t hz = (uint32_t)strtoul(argv[1], NULL, 10);
	int hold = (argc > 2) ? atoi(argv[2]) : 0; /* 0 = until Ctrl-C */

	st7789_config_t cfg;
	st7789_config_default(&cfg);
	cfg.spi_hz = hz;

	st7789_tune_system(65536);

	st7789_t *dev = st7789_open(&cfg);
	if(!dev) {
		perror("st7789_open");
		return 1;
	}
	int w = st7789_get_width(dev);
	int h = st7789_get_height(dev);

	uint16_t *fb = malloc((size_t)w * h * sizeof(uint16_t));
	if(!fb) {
		st7789_close(dev);
		return 1;
	}

	/* Color bars across the width. */
	const uint16_t bars[] = {
	    0xF800, /* red   */
	    0x07E0, /* green */
	    0x001F, /* blue  */
	    0xFFFF, /* white */
	    0xFFE0, /* yellow*/
	    0x07FF, /* cyan  */
	    0xF81F, /* magenta */
	    0x0000, /* black */
	};
	int nbars = (int)(sizeof(bars) / sizeof(bars[0]));

	for(int y = 0; y < h; y++) {
		for(int x = 0; x < w; x++) {
			uint16_t c;
			if(y < h / 2) {
				/* top: color bars */
				c = bars[(x * nbars) / w];
			} else {
				/* bottom: horizontal grayscale gradient */
				uint8_t g = (uint8_t)(x * 255 / (w - 1));
				c = st7789_rgb565(g, g, g);
			}
			/* 1px grid every 16px (both axes) in a mid color to reveal shifts */
			if((x % 16 == 0) || (y % 16 == 0)) c = 0x8410; /* gray */
			fb[y * w + x] = c;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if(hold < 0) {
		/* Continuous re-push: same image every frame. Any shimmer = instability. */
		printf("continuous refresh at requested %u Hz (%dx%d). "
		       "Watch for shimmer/flicker = unstable. Ctrl-C to stop.\n",
		       hz, w, h);
		unsigned long n = 0, fail = 0;
		while(!g_stop) {
			if(st7789_flush(dev, fb) < 0) fail++;
			if((++n & 0x3FF) == 0) {
				printf("\rframes=%lu  flush_errors=%lu   ", n, fail);
				fflush(stdout);
			}
		}
		printf("\nstopped: frames=%lu flush_errors=%lu\n", n, fail);
	} else {
		if(st7789_flush(dev, fb) < 0) {
			fprintf(stderr, "flush failed at %u Hz\n", hz);
			free(fb);
			st7789_close(dev);
			return 1;
		}
		printf("pattern pushed at requested %u Hz (%dx%d). "
		       "Inspect for color noise / broken grid lines.\n",
		       hz, w, h);
		if(hold > 0) {
			sleep((unsigned)hold);
		} else {
			printf("holding... press Ctrl-C to exit (screen keeps this image)\n");
			while(!g_stop) pause();
		}
	}

	free(fb);
	st7789_close(dev);
	return 0;
}
