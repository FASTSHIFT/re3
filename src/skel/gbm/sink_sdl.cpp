/*
 * sink_sdl.cpp - OutputSink presenting frames in an SDL2 window (desktop).
 *
 * Debug/emulator path: run the exact GBM render pipeline (EGL+GBM surfaceless
 * GLES2 + glReadPixels) on a desktop and show the readback in a window. Lets us
 * reproduce and debug Pi-only issues (z-order, streaming crashes) on a fast
 * machine without the panel. Presents at the render resolution, upscaled to a
 * larger window for visibility.
 *
 * Input is handled by input_sdl.cpp (separate source); this file only pumps
 * SDL_QuitEvent so the window close button works.
 *
 * Active when RE3_OUTPUT_SDL is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_OUTPUT_SDL)

#include "output_sink.h"

#include <stdio.h>
#include <stdlib.h>

#include <SDL2/SDL.h>

#include "common.h"
#include "skeleton.h"

static SDL_Window *sWin = 0;
static SDL_Renderer *sRen = 0;
static SDL_Texture *sTex = 0;
static uint8_t *sRGBX = 0; // flipped RGBX8888 scratch (render size)
static int sW = 0, sH = 0;

// Window scale factor. Default 1:1; override with RE3_SDL_SCALE for a larger
// debug window when the render resolution is small (e.g. 320x240).
static int sScale = 1;

static bool
sdl_init(int renderW, int renderH)
{
	// librw already holds an EGL GLES context current on this thread. Keep SDL
	// entirely off GL/GLX: use the software framebuffer path, no GL renderer.
	// Otherwise SDL creates a GLX context on the same X display and clashes
	// (BadAccess on X_GLXMakeCurrent).
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");

	if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		printf("sdl: SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
		return false;
	}

	const char *e = getenv("RE3_SDL_SCALE");
	if(e != 0) sScale = atoi(e);
	if(sScale < 1) sScale = 1;

	sW = renderW;
	sH = renderH;
	sWin = SDL_CreateWindow("re3 (GBM/SDL debug)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, renderW * sScale, renderH * sScale, SDL_WINDOW_SHOWN);
	if(sWin == 0) {
		printf("sdl: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}
	// Force the software renderer: librw already owns an EGL GLES context on
	// this thread, and an accelerated SDL renderer would create a second GL
	// (GLX) context on the same display and clash (BadAccess on MakeCurrent).
	// We only blit a texture, so software is fine.
	sRen = SDL_CreateRenderer(sWin, -1, SDL_RENDERER_SOFTWARE);
	if(sRen == 0) {
		printf("sdl: SDL_CreateRenderer(software) failed: %s\n", SDL_GetError());
		return false;
	}
	sTex = SDL_CreateTexture(sRen, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, renderW, renderH);
	if(sTex == 0) {
		printf("sdl: SDL_CreateTexture failed: %s\n", SDL_GetError());
		return false;
	}
	sRGBX = (uint8_t *)malloc((size_t)renderW * renderH * 4);
	printf("sdl: window %dx%d (x%d) render %dx%d\n", renderW * sScale, renderH * sScale, sScale, renderW, renderH);
	return sRGBX != 0;
}

static void
sdl_present(const uint16_t *rgb565, int w, int h)
{
	if(sRen == 0 || sTex == 0 || sRGBX == 0) return;

	// Handle only window-close here. Do NOT drain all events: the input source
	// (input_sdl.cpp) peeks key/mouse/wheel events, so we must leave those in
	// the queue. Pump once, then take only SDL_QUIT.
	SDL_PumpEvents();
	SDL_Event qev[4];
	if(SDL_PeepEvents(qev, 4, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) RsGlobal.quit = TRUE;

	// Expand RGB565 -> RGB888 and flip bottom-up GL readback to top-down.
	// SDL_PIXELFORMAT_RGB888 on a little-endian host is stored as B,G,R,X bytes.
	for(int y = 0; y < h; y++) {
		const uint16_t *srcRow = rgb565 + (h - 1 - y) * w;
		uint8_t *dstRow = sRGBX + y * w * 4;
		for(int x = 0; x < w; x++) {
			uint16_t p = srcRow[x];
			dstRow[x * 4 + 0] = (uint8_t)((p & 0x1F) << 3);         // B
			dstRow[x * 4 + 1] = (uint8_t)(((p >> 5) & 0x3F) << 2);  // G
			dstRow[x * 4 + 2] = (uint8_t)(((p >> 11) & 0x1F) << 3); // R
			dstRow[x * 4 + 3] = 0xFF;
		}
	}
	SDL_UpdateTexture(sTex, 0, sRGBX, w * 4);
	SDL_RenderClear(sRen);
	SDL_RenderCopy(sRen, sTex, 0, 0);
	SDL_RenderPresent(sRen);

	// Debug: dump frames to PPM when RE3_SDL_DUMP=<dir> (headless verification
	// when the X window can't be screenshotted). Writes frameNNNN.ppm.
	static const char *dumpDir = 0;
	static int dumpInit = 0, dumpN = 0, dumpEvery = 60;
	if(!dumpInit) {
		dumpInit = 1;
		dumpDir = getenv("RE3_SDL_DUMP");
		const char *ev = getenv("RE3_SDL_DUMP_EVERY");
		if(ev) dumpEvery = atoi(ev);
		if(dumpEvery < 1) dumpEvery = 1;
	}
	if(dumpDir != 0 && (dumpN % dumpEvery) == 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s/frame%04d.ppm", dumpDir, dumpN / dumpEvery);
		FILE *f = fopen(path, "wb");
		if(f) {
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for(int y = 0; y < h; y++) {
				const uint16_t *srcRow = rgb565 + (h - 1 - y) * w; // flip
				for(int x = 0; x < w; x++) {
					uint16_t p = srcRow[x];
					uint8_t rgb[3];
					rgb[0] = (uint8_t)(((p >> 11) & 0x1F) << 3);
					rgb[1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
					rgb[2] = (uint8_t)((p & 0x1F) << 3);
					fwrite(rgb, 1, 3, f);
				}
			}
			fclose(f);
		}
	}
	dumpN++;
}

static void
sdl_terminate(void)
{
	if(sRGBX) {
		free(sRGBX);
		sRGBX = 0;
	}
	if(sTex) {
		SDL_DestroyTexture(sTex);
		sTex = 0;
	}
	if(sRen) {
		SDL_DestroyRenderer(sRen);
		sRen = 0;
	}
	if(sWin) {
		SDL_DestroyWindow(sWin);
		sWin = 0;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static OutputSink sSink = {sdl_init, sdl_present, sdl_terminate, "sdl"};

OutputSink *
OutputSink_Get(void)
{
	return &sSink;
}

#endif
