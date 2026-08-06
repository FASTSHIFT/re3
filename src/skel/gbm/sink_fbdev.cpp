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
fbdev_dump(const uint16_t *rgb565, int w, int h)
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
				const uint16_t *srcRow = rgb565 + (h - 1 - y) * w;	// flip
				for (int x = 0; x < w; x++) {
					uint16_t p = srcRow[x];
					uint8_t rgb[3];
					rgb[0] = (uint8_t)(((p >> 11) & 0x1F) << 3);
					rgb[1] = (uint8_t)(((p >> 5)  & 0x3F) << 2);
					rgb[2] = (uint8_t)(( p        & 0x1F) << 3);
					fwrite(rgb, 1, 3, f);
				}
			}
			fclose(f);
		}
	}
	n++;
}

static void
fbdev_present(const uint16_t *rgb565, int w, int h)
{
	fbdev_dump(rgb565, w, h);

	if (sMem == 0)
		return;	// no display; readback already exercised the pipeline

	int ox = ((int)sVar.xres - w) / 2;
	int oy = ((int)sVar.yres - h) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;

	// The frame is already RGB565. On a 16bpp fb it's a straight row copy (with
	// vertical flip, fb is top-down). On 32bpp we expand 565 -> 888.
	if (sVar.bits_per_pixel == 16) {
		for (int y = 0; y < h; y++) {
			const uint16_t *srcRow = rgb565 + (h - 1 - y) * w;
			uint16_t *dst = (uint16_t *)(sMem + (oy + y) * sFix.line_length + ox * 2);
			memcpy(dst, srcRow, (size_t)w * 2);
		}
	} else if (sVar.bits_per_pixel == 32) {
		for (int y = 0; y < h; y++) {
			const uint16_t *srcRow = rgb565 + (h - 1 - y) * w;
			uint32_t *dst = (uint32_t *)(sMem + (oy + y) * sFix.line_length) + ox;
			for (int x = 0; x < w; x++) {
				uint16_t p = srcRow[x];
				uint8_t r = (uint8_t)(((p >> 11) & 0x1F) << 3);
				uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
				uint8_t b = (uint8_t)(( p        & 0x1F) << 3);
				dst[x] = (r << 16) | (g << 8) | b;
			}
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
