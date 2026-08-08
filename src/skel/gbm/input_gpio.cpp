/*
 * input_gpio.cpp - GPIO physical-button input (docs/08 method A).
 * ...
 * Active in SPI builds (RE3_OUTPUT_SPI). Compatible with evdev being also active.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_OUTPUT_SPI)

#include "input_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "common.h"
#include "Pad.h"
#include "Frontend.h" // FrontEndMenuManager.m_bMenuActive
#include "skeleton.h" // RsKeyboardEventHandler, rsESC
#include "pi_gpio.h"

// ---- pin table ------------------------------------------------------------
struct GpioKey {
	const char *env;
	int pin;
};
enum {
	GK_UP,
	GK_DOWN,
	GK_LEFT,
	GK_RIGHT,
	GK_Y,
	GK_A,
	GK_X,
	GK_B, // face buttons (separate from dpad)
	GK_SELECT,
	GK_START,
	GK_L,
	GK_R,
	GK_COUNT
};
static GpioKey sKeys[GK_COUNT] = {
    {"RE3_KEY_UP", 12},     // dpad up    -> LeftStick up + DPadUp
    {"RE3_KEY_DOWN", 20},   // dpad down  -> LeftStick down + DPadDown
    {"RE3_KEY_LEFT", 21},   // dpad left  -> LeftStick left + DPadLeft
    {"RE3_KEY_RIGHT", 13},  // dpad right -> LeftStick right + DPadRight
    {"RE3_KEY_Y", 17},      // face Y     -> RightStick up (in-game) / DPadUp (menu)
    {"RE3_KEY_A", 23},      // face A     -> RightStick down / DPadDown
    {"RE3_KEY_X", 22},      // face X     -> RightStick left / DPadLeft
    {"RE3_KEY_B", 4},       // face B     -> RightStick right / DPadRight
    {"RE3_KEY_SELECT", 16}, // SELECT     -> Triangle (enter/exit vehicle)
    {"RE3_KEY_START", 26},  // START      -> Cross (confirm); long->ESC
    {"RE3_KEY_L", 5},       // L shoulder -> Circle (fire/shoot)
    {"RE3_KEY_R", 6}        // R shoulder -> LeftShoulder1 (aim/target)
};
static bool sReady = false;

// Long-press START tracking: hold >600ms triggers ESC.
#define START_LONG_MS 600
static int sStartWasDown = 0;
static double sStartDownAt = 0.0;
static int sStartLongFired = 0; // ESC injected, don't also send Cross

static double
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---- init -----------------------------------------------------------------
static void
gpio_init(void)
{
	if(pi_gpio_init() < 0) {
		printf("gpio: pi_gpio_init failed; input unavailable\n");
		return;
	}
	for(int i = 0; i < GK_COUNT; i++) {
		const char *e = getenv(sKeys[i].env);
		if(e != 0) sKeys[i].pin = atoi(e);
		pi_gpio_set_mode((uint8)sKeys[i].pin, PI_GPIO_INPUT);
		pi_gpio_set_pull((uint8)sKeys[i].pin, PI_GPIO_PULL_UP);
	}
	sReady = true;
	printf("gpio: input ready\n");
	printf("  dpad  UP=%d DN=%d LT=%d RT=%d\n", sKeys[GK_UP].pin, sKeys[GK_DOWN].pin, sKeys[GK_LEFT].pin, sKeys[GK_RIGHT].pin);
	printf("  face  Y=%d A=%d X=%d B=%d\n", sKeys[GK_Y].pin, sKeys[GK_A].pin, sKeys[GK_X].pin, sKeys[GK_B].pin);
	printf("  misc  SEL=%d STA=%d L=%d R=%d\n", sKeys[GK_SELECT].pin, sKeys[GK_START].pin, sKeys[GK_L].pin, sKeys[GK_R].pin);
}

static inline bool
p(int idx)
{
	return pi_gpio_get_value((uint8)sKeys[idx].pin) == 0;
}

// ---- capturePad -----------------------------------------------------------
static void
gpio_capturePad(int padID)
{
	if(padID != 0 || !sReady) return;

	CPad *pad = CPad::GetPad(0);
	CControllerState &s = pad->PCTempJoyState;
	s.Clear();

	bool inMenu = !!FrontEndMenuManager.m_bMenuActive;

	// --- D-pad: always controls movement (LeftStick) and menu nav (DPad). ---
	bool du = p(GK_UP), dd = p(GK_DOWN), dl = p(GK_LEFT), dr = p(GK_RIGHT);
	s.DPadUp = du ? 255 : 0;
	s.DPadDown = dd ? 255 : 0;
	s.DPadLeft = dl ? 255 : 0;
	s.DPadRight = dr ? 255 : 0;
	s.LeftStickX = (int16)((dr ? 128 : 0) - (dl ? 128 : 0));
	s.LeftStickY = (int16)((dd ? 128 : 0) - (du ? 128 : 0));

	// --- Face buttons YAXB ---
	bool fy = p(GK_Y), fa = p(GK_A), fx = p(GK_X), fb = p(GK_B);
	if(inMenu) {
		// In menus, YAXB = DPad navigation (up/down/left/right).
		// OR into the DPad that the dpad already set.
		if(fy) { s.DPadUp = 255; }
		if(fa) { s.DPadDown = 255; }
		if(fx) { s.DPadLeft = 255; }
		if(fb) { s.DPadRight = 255; }
	} else {
		// In-game: YAXB = camera look via RightStick.
		// Y=up, A=down, X=left, B=right (full deflection ±128).
		s.RightStickY = (int16)((fa ? 128 : 0) - (fy ? 128 : 0));
		s.RightStickX = (int16)((fb ? 128 : 0) - (fx ? 128 : 0));
	}

	// --- Shoulders ---
	// L = fire/shoot (Circle), R = aim/target (LeftShoulder1)
	s.Circle = p(GK_L) ? 255 : 0;
	s.LeftShoulder1 = p(GK_R) ? 255 : 0;

	// --- SELECT = Triangle (enter/exit vehicle, interact) ---
	s.Triangle = p(GK_SELECT) ? 255 : 0;

	// --- START = Cross (confirm/jump); long-press -> ESC (back/pause) ---
	bool startNow = p(GK_START);
	if(startNow && !sStartWasDown) {
		// just pressed
		sStartDownAt = mono_ms();
		sStartLongFired = 0;
	}
	if(startNow && !sStartLongFired) {
		double held = mono_ms() - sStartDownAt;
		if(held >= START_LONG_MS) {
			// Long press: inject ESC and mark so we don't also send Cross.
			int esc = rsESC;
			RsKeyboardEventHandler(rsKEYDOWN, &esc);
			RsKeyboardEventHandler(rsKEYUP, &esc);
			sStartLongFired = 1;
		}
	}
	if(!startNow && sStartWasDown && !sStartLongFired) {
		// Short tap released: send Cross (confirm) on release.
		s.Cross = 255;
	}
	sStartWasDown = startNow ? 1 : 0;
}

static void
gpio_poll(void)
{
}
static void
gpio_terminate(void)
{
}

static InputSource sSrc = {gpio_init, gpio_poll, gpio_terminate, gpio_capturePad, "gpio"};

// Self-register so the registry dispatches to us regardless of other sources.
// Use a static init trick via a dummy variable to run at startup.
static struct GpioAutoReg {
	GpioAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
