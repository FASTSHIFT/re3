/*
 * output_sink.h - pluggable frame output for the headless (GBM) skeleton.
 *
 * The GBM skeleton renders the scene offscreen (EGL+GBM, hardware GLES2) and
 * reads it back once per frame. Where that frame goes is a pluggable "sink":
 *   - fbdev : /dev/fb0 (HDMI debug)
 *   - spi   : ST7789 SPI panel (drivers/st7789)
 *   - sdl   : an SDL2 window (desktop/emulator debugging)
 *
 * The skeleton owns the glReadPixels; a sink only receives the RGBA8 frame and
 * presents it. This keeps the platform-independent layer and gbm.cpp small and
 * makes each backend independently testable. See docs/08.
 */
#ifndef RE3_OUTPUT_SINK_H
#define RE3_OUTPUT_SINK_H

#include <stdint.h>

struct OutputSink
{
	// Called once after RW init with the render resolution. Returns true on
	// success. A sink may present at a different resolution than the render
	// size (e.g. a fixed panel); the skeleton scales into what present() wants.
	bool (*init)(int renderW, int renderH);

	// Present one rendered frame. 'rgb565' is renderW*renderH little-endian
	// RGB565 (the GBM camera renders directly in this display-native format),
	// bottom-up (GL origin bottom-left). Sinks flip/scale as needed; fb0(16bpp)
	// and ST7789 consume it directly, the SDL debug sink expands to RGB888.
	void (*present)(const uint16_t *rgb565, int renderW, int renderH);

	// Release resources. Safe to call if init() failed.
	void (*terminate)(void);

	const char *name;
};

// Select the sink at build time. Exactly one is compiled/active per build:
//   RE3_OUTPUT_SPI -> spi, RE3_OUTPUT_SDL -> sdl, otherwise -> fbdev.
// Returns a static sink (never null).
OutputSink *OutputSink_Get(void);

#endif /* RE3_OUTPUT_SINK_H */
