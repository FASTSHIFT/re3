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
#include "pi_gpio.h"

// Forward-declared to avoid pulling in PlayerInfo.h (which drags in CPtrList
// and other engine headers). Returns the player's current vehicle, or nil when
// on foot. Declared in PlayerInfo.h / defined in World.cpp.
class CVehicle;
CVehicle *
FindPlayerVehicle(void);

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

// Monotonic millisecond clock. uint32 wraps every ~49.7 days, but we only ever
// take differences of nearby timestamps and unsigned subtraction is wrap-safe,
// so ms resolution in 32 bits is plenty here.
static uint32
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// R-shoulder double-tap tracking (on foot): hold = aim, double-tap = cycle
// weapon. A double tap is two short presses in quick succession.
#define R_TAP_MAX_MS 250u  // a press shorter than this counts as a "tap"
#define R_DTAP_GAP_MS 300u // max gap between the two taps' presses
static int sRWasDown = 0;
static int sRHaveTap = 0;      // a qualifying first tap is pending
static uint32 sRDownAt = 0;    // when R was last pressed
static uint32 sRLastTapAt = 0; // when the previous qualifying tap was pressed

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

	// PCTempJoyState is cleared once by InputSource_CapturePadAll before any
	// source runs; we OR our contribution in (see input_registry.cpp).
	CPad *pad = CPad::GetPad(0);
	CControllerState &s = pad->PCTempJoyState;

	bool inMenu = !!FrontEndMenuManager.m_bMenuActive;
	// "On foot" = actually in the game world and not in a vehicle. Only then do
	// the face buttons act as a camera stick; in menus and while driving they
	// behave as normal face buttons (so e.g. Cross = accelerate works in a car).
	bool onFoot = !inMenu && FindPlayerVehicle() == nil;

	// All writes below are OR-only (only set fields for buttons that are
	// actually held). PCTempJoyState is cleared once by the registry before
	// any source runs, so writing zeros here would clobber a co-active evdev
	// controller instead of leaving its contribution intact.

	// --- D-pad: always controls movement (LeftStick) and menu nav (DPad). ---
	bool du = p(GK_UP), dd = p(GK_DOWN), dl = p(GK_LEFT), dr = p(GK_RIGHT);
	if(du) s.DPadUp = 255;
	if(dd) s.DPadDown = 255;
	if(dl) s.DPadLeft = 255;
	if(dr) s.DPadRight = 255;
	if(dl || dr) s.LeftStickX = (int16)((dr ? 128 : 0) - (dl ? 128 : 0));
	if(du || dd) s.LeftStickY = (int16)((dd ? 128 : 0) - (du ? 128 : 0));

	// --- Face buttons YAXB ---
	bool fy = p(GK_Y), fa = p(GK_A), fx = p(GK_X), fb = p(GK_B);
	if(onFoot) {
		// On foot only: YAXB = camera look via RightStick.
		// Y=up, A=down, X=left, B=right (full deflection ±128).
		if(fy || fa) s.RightStickY = (int16)((fa ? 128 : 0) - (fy ? 128 : 0));
		if(fx || fb) s.RightStickX = (int16)((fb ? 128 : 0) - (fx ? 128 : 0));
	} else {
		// Menus and in-vehicle: YAXB = normal PlayStation face buttons.
		// Y=Triangle, A=Cross, X=Square, B=Circle. In a car this makes
		// A=Cross=accelerate, B=Circle=brake/reverse work as expected; in
		// menus Cross confirms and Triangle/Circle back out.
		if(fy) s.Triangle = 255;
		if(fa) s.Cross = 255;
		if(fx) s.Square = 255;
		if(fb) s.Circle = 255;
	}

	// --- Shoulders ---
	// L shoulder:
	//   on foot      -> fire/shoot (Circle).
	//   in a vehicle -> horn. With the default control config (Mode 0) the
	//                   horn is read from LeftShock (see CPad::GetHorn), so map
	//                   L to LeftShock while driving.
	if(p(GK_L)) {
		if(onFoot)
			s.Circle = 255;
		else
			s.LeftShock = 255;
	}

	// R shoulder:
	//   in a vehicle -> aim/target (LeftShoulder1), as before.
	//   on foot      -> HOLD aims (LeftShoulder1, no delay); a DOUBLE-TAP
	//                   cycles to the next weapon (RightShoulder2 for one
	//                   frame). Aiming is never delayed; the two brief taps of
	//                   a double-tap just also register a frame of aim, which
	//                   is imperceptible.
	bool rNow = p(GK_R);
	if(!onFoot) {
		if(rNow) s.LeftShoulder1 = 255;
	} else {
		if(rNow && !sRWasDown) {
			// press edge: is this the 2nd tap of a double-tap?
			uint32 now = mono_ms();
			if(sRHaveTap && (uint32)(now - sRLastTapAt) <= R_DTAP_GAP_MS) {
				s.RightShoulder2 = 255; // cycle weapon (one frame)
				sRHaveTap = 0;          // consume, don't triple-trigger
			}
			sRDownAt = now;
		}
		if(!rNow && sRWasDown) {
			// release edge: a short press qualifies as a tap for next time.
			if((uint32)(mono_ms() - sRDownAt) <= R_TAP_MAX_MS) {
				sRLastTapAt = sRDownAt;
				sRHaveTap = 1;
			}
		}
		if(rNow) s.LeftShoulder1 = 255; // hold = aim (immediate)
	}
	sRWasDown = rNow ? 1 : 0;

	// --- SELECT = Triangle (enter/exit vehicle, interact) ---
	if(p(GK_SELECT)) s.Triangle = 255;

	// --- START = Start (pause/resume the game; REGISTER_START_BUTTON). ---
	// Also drives our metrics-overlay toggle on each pause (see below).
	if(p(GK_START)) s.Start = 255;
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
