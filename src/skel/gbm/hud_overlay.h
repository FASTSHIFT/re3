/*
 * hud_overlay.h - tiny on-frame metrics overlay for the headless GBM skeleton.
 *
 * Draws perf metrics (FPS, CPU/GPU/copy ms, CPU temp) directly onto the RGB565
 * readback buffer before it is presented, so it shows on fb0 / SPI without the
 * game's font system. Enabled with the RE3_HUD=1 environment variable.
 */
#ifndef RE3_HUD_OVERLAY_H
#define RE3_HUD_OVERLAY_H

#include <stdint.h>

// Per-frame timings fed by the skeleton (milliseconds).
struct HudMetrics {
	double frameMs;		// wall time between frames
	double cpuMs;		// game CPU (RsEventHandler rsIDLE) time
	double gpuMs;		// showRaster/glFinish (GPU scene wait)
	double readMs;		// glReadPixels readback
	double presentMs;	// sink present (blit/scale/flush)

	// Clock frequencies (MHz). Filled by the skeleton from sysfs; 0 = unknown.
	int armMhz;		// ARM CPU core (e.g. 1000)
	int v3dMhz;		// V3D GPU shader core (e.g. 300)
};

// Is the HUD enabled? (checks RE3_HUD once). Cheap to call per frame.
int Hud_Enabled(void);

// Feed the latest per-frame metrics (call once per frame).
void Hud_Update(const HudMetrics *m);

// Draw the HUD onto an RGB565 framebuffer (w x h, top-down row order as stored
// in the readback buffer's memory). Call from the sink/skeleton just before
// present. No-op if disabled.
void Hud_Draw(uint16_t *rgb565, int w, int h);

#endif /* RE3_HUD_OVERLAY_H */
