/*
 * sink_fbdev.cpp - OutputSink writing to /dev/fb0 (Linux framebuffer).
 *
 * Debug/verification path (docs/07): blit the readback frame to fb0, centered,
 * for 16bpp (RGB565) and 32bpp displays. When fb0 is absent (no HDMI) the sink
 * still succeeds so the render+readback pipeline is exercised.
 *
 * Active when neither RE3_OUTPUT_SPI nor RE3_OUTPUT_SDL is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && !defined(RE3_OUTPUT_SPI) && !defined(RE3_OUTPUT_SDL)

#include "output_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

static int		sFd = -1;
static uint8_t	*sMem = 0;
static size_t	sSize = 0;
static struct fb_var_screeninfo sVar;
static struct fb_fix_screeninfo sFix;

static bool
fbdev_init(int renderW, int renderH)
{
	(void)renderW; (void)renderH;
	sFd = open("/dev/fb0", O_RDWR);
	if (sFd < 0) {
		printf("fbdev: /dev/fb0 unavailable (no HDMI?) - render+readback only\n");
		return true;	// not fatal; pipeline still runs
	}
	if (ioctl(sFd, FBIOGET_VSCREENINFO, &sVar) != 0 ||
		ioctl(sFd, FBIOGET_FSCREENINFO, &sFix) != 0) {
		printf("fbdev: ioctl failed\n");
		close(sFd); sFd = -1;
		return true;
	}
	printf("fbdev: /dev/fb0 %dx%d %dbpp line=%d\n", sVar.xres, sVar.yres,
		sVar.bits_per_pixel, sFix.line_length);
	sSize = sFix.line_length * sVar.yres;
	sMem = (uint8_t *)mmap(0, sSize, PROT_READ | PROT_WRITE, MAP_SHARED, sFd, 0);
	if (sMem == MAP_FAILED) {
		printf("fbdev: mmap failed\n");
		sMem = 0; close(sFd); sFd = -1;
	}
	return true;
}

// Debug: dump frames to PPM when RE3_FB_DUMP=<dir> (headless verification when
// fb0 can't be seen, e.g. the desktop owns it). Writes frameNNNN.ppm.
static void
fbdev_dump(const uint8_t *rgba, int w, int h)
{
	static const char *dir = 0;
	static int init = 0, n = 0, every = 60;
	if (!init) {
		init = 1;
		dir = getenv("RE3_FB_DUMP");
		const char *ev = getenv("RE3_FB_DUMP_EVERY");
		if (ev) every = atoi(ev);
		if (every < 1) every = 1;
	}
	if (dir == 0) return;
	if ((n % every) == 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s/frame%04d.ppm", dir, n / every);
		FILE *f = fopen(path, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int y = 0; y < h; y++) {
				const uint8_t *srcRow = rgba + (h - 1 - y) * w * 4;	// flip
				for (int x = 0; x < w; x++)
					fwrite(srcRow + x * 4, 1, 3, f);
			}
			fclose(f);
		}
	}
	n++;
}

static void
fbdev_present(const uint8_t *rgba, int w, int h)
{
	fbdev_dump(rgba, w, h);

	if (sMem == 0)
		return;	// no display; readback already exercised the pipeline

	int ox = ((int)sVar.xres - w) / 2;
	int oy = ((int)sVar.yres - h) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;

	// GL image is bottom-up; flip vertically while blitting (fb is top-down).
	if (sVar.bits_per_pixel == 16) {
		for (int y = 0; y < h; y++) {
			const uint8_t *srcRow = rgba + (h - 1 - y) * w * 4;
			uint16_t *dst = (uint16_t *)(sMem + (oy + y) * sFix.line_length + ox * 2);
			for (int x = 0; x < w; x++) {
				uint8_t r = srcRow[x*4+0], g = srcRow[x*4+1], b = srcRow[x*4+2];
				dst[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
			}
		}
	} else if (sVar.bits_per_pixel == 32) {
		for (int y = 0; y < h; y++) {
			const uint8_t *srcRow = rgba + (h - 1 - y) * w * 4;
			uint32_t *dst = (uint32_t *)(sMem + (oy + y) * sFix.line_length) + ox;
			for (int x = 0; x < w; x++)
				dst[x] = (srcRow[x*4+0] << 16) | (srcRow[x*4+1] << 8) | srcRow[x*4+2];
		}
	}
}

static void
fbdev_terminate(void)
{
	if (sMem != 0 && sSize != 0)
		munmap(sMem, sSize);
	if (sFd >= 0)
		close(sFd);
	sMem = 0; sFd = -1; sSize = 0;
}

static OutputSink sSink = { fbdev_init, fbdev_present, fbdev_terminate, "fbdev" };

OutputSink *OutputSink_Get(void) { return &sSink; }

#endif
