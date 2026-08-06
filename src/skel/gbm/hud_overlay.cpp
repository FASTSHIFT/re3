/*
 * hud_overlay.cpp - metrics overlay drawn into the RGB565 readback buffer.
 *
 * Self-contained 5x7 bitmap font (no dependency on the game's font system) so
 * it renders identically on fb0 and the SPI panel. Reads CPU temperature from
 * /sys/class/thermal (to catch thermal throttling). See hud_overlay.h.
 */
#if defined RW_GL3 && defined LIBRW_GBM

#include "hud_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- 5x7 bitmap font ------------------------------------------------------
// Each glyph is 5 columns x 7 rows, one byte per column (low 7 bits = rows,
// bit0 = top). Only the characters used by the HUD are defined.
struct Glyph { char c; uint8_t col[5]; };

static const Glyph kFont[] = {
	{' ', {0x00,0x00,0x00,0x00,0x00}},
	{'0', {0x3E,0x51,0x49,0x45,0x3E}},
	{'1', {0x00,0x42,0x7F,0x40,0x00}},
	{'2', {0x42,0x61,0x51,0x49,0x46}},
	{'3', {0x21,0x41,0x45,0x4B,0x31}},
	{'4', {0x18,0x14,0x12,0x7F,0x10}},
	{'5', {0x27,0x45,0x45,0x45,0x39}},
	{'6', {0x3C,0x4A,0x49,0x49,0x30}},
	{'7', {0x01,0x71,0x09,0x05,0x03}},
	{'8', {0x36,0x49,0x49,0x49,0x36}},
	{'9', {0x06,0x49,0x49,0x29,0x1E}},
	{'.', {0x00,0x60,0x60,0x00,0x00}},
	{':', {0x00,0x36,0x36,0x00,0x00}},
	{'%', {0x23,0x13,0x08,0x64,0x62}},
	{'-', {0x08,0x08,0x08,0x08,0x08}},
	{'(', {0x00,0x1C,0x22,0x41,0x00}},
	{')', {0x00,0x41,0x22,0x1C,0x00}},
	{'C', {0x3E,0x41,0x41,0x41,0x22}},
	{'P', {0x7F,0x09,0x09,0x09,0x06}},
	{'U', {0x3F,0x40,0x40,0x40,0x3F}},
	{'G', {0x3E,0x41,0x49,0x49,0x7A}},
	{'F', {0x7F,0x09,0x09,0x09,0x01}},
	{'S', {0x46,0x49,0x49,0x49,0x31}},
	{'T', {0x01,0x01,0x7F,0x01,0x01}},
	{'M', {0x7F,0x02,0x0C,0x02,0x7F}},
	{'P', {0x7F,0x09,0x09,0x09,0x06}},
	{'Y', {0x07,0x08,0x70,0x08,0x07}},
	{'D', {0x7F,0x41,0x41,0x22,0x1C}},
	{'W', {0x3F,0x40,0x38,0x40,0x3F}},
	{'R', {0x7F,0x09,0x19,0x29,0x46}},
};

static const uint8_t *glyph(char c)
{
	for (int i = 0; i < (int)(sizeof(kFont)/sizeof(kFont[0])); i++)
		if (kFont[i].c == c) return kFont[i].col;
	return kFont[0].col;	// space
}

static int sEnabled = -1;

int Hud_Enabled(void)
{
	if (sEnabled < 0)
		sEnabled = getenv("RE3_HUD") ? 1 : 0;
	return sEnabled;
}

// ---- metrics --------------------------------------------------------------
static HudMetrics sM;
static char sLines[6][40];
static int sNumLines = 0;

static int read_cpu_temp_milli(void)
{
	static int failed = 0;
	if (failed) return -1;
	FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	if (!f) { failed = 1; return -1; }
	int t = -1;
	if (fscanf(f, "%d", &t) != 1) t = -1;
	fclose(f);
	return t;
}

void Hud_Update(const HudMetrics *m)
{
	if (!Hud_Enabled()) return;
	sM = *m;

	// Refresh temperature every ~30 frames (sysfs read is not free).
	static int tempMilli = -1, cnt = 0;
	if ((cnt++ % 30) == 0)
		tempMilli = read_cpu_temp_milli();

	double fps = sM.frameMs > 0.0 ? 1000.0 / sM.frameMs : 0.0;

	sNumLines = 0;
	snprintf(sLines[sNumLines++], sizeof(sLines[0]), "FPS: %.0f (%.0fMS)", fps, sM.frameMs);
	snprintf(sLines[sNumLines++], sizeof(sLines[0]), "CPU: %.1f", sM.cpuMs);
	snprintf(sLines[sNumLines++], sizeof(sLines[0]), "GPU: %.1f", sM.gpuMs);
	snprintf(sLines[sNumLines++], sizeof(sLines[0]), "CPY: %.1f", sM.readMs + sM.presentMs);
	if (tempMilli >= 0)
		snprintf(sLines[sNumLines++], sizeof(sLines[0]), "TMP: %.1fC", tempMilli / 1000.0);
}

// ---- drawing --------------------------------------------------------------
static inline void putpx(uint16_t *fb, int w, int h, int x, int y, uint16_t c)
{
	if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
		fb[y * w + x] = c;
}

// Draw one char at (x,y), scale s, colour c. Returns advance in pixels.
// The target is the GL readback buffer, which is stored bottom-up; the sink
// flips it vertically before display. So emit each glyph row-flipped (6-row)
// here, and place lines from the buffer bottom, so text reads upright on screen.
static int draw_char(uint16_t *fb, int w, int h, int x, int y, char ch, int s, uint16_t c)
{
	const uint8_t *g = glyph(ch);
	for (int col = 0; col < 5; col++) {
		uint8_t bits = g[col];
		for (int row = 0; row < 7; row++) {
			if (bits & (1 << row)) {
				for (int sy = 0; sy < s; sy++)
					for (int sx = 0; sx < s; sx++)
						putpx(fb, w, h, x + col*s + sx, y + (6 - row)*s + sy, c);
			}
		}
	}
	return (5 + 1) * s;
}

static void draw_text(uint16_t *fb, int w, int h, int x, int y, const char *str, int s, uint16_t c)
{
	for (const char *p = str; *p; p++) {
		char ch = *p;
		if (ch >= 'a' && ch <= 'z') ch -= 32;	// font is uppercase only
		x += draw_char(fb, w, h, x, y, ch, s, c);
	}
}

void Hud_Draw(uint16_t *rgb565, int w, int h)
{
	if (!Hud_Enabled() || rgb565 == 0 || sNumLines == 0)
		return;

	const int s = (w >= 480) ? 2 : 1;	// larger text on bigger targets
	const int glyphH = 7 * s;
	const int lineH = 8 * s + 1;
	const uint16_t fg = 0xFFFF;	// white
	const uint16_t bg = 0x0000;	// black shadow

	// Buffer is bottom-up; to make the block sit at the screen's TOP-LEFT and
	// read top-to-bottom, place line i at buffer-y counting down from the top
	// of the buffer (= screen top), with the first line highest on screen.
	for (int i = 0; i < sNumLines; i++) {
		int y = h - 1 - glyphH - i * lineH;	// screen-top anchor (buffer top)
		if (y < 0) break;
		draw_text(rgb565, w, h, 2, y - 1, sLines[i], s, bg);	// shadow
		draw_text(rgb565, w, h, 1, y,     sLines[i], s, fg);
	}
}

#endif
