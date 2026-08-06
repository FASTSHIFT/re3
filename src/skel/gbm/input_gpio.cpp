/*
 * input_gpio.cpp - InputSource for GPIO physical buttons (docs/08 method A).
 *
 * Treats the buttons as a virtual gamepad: init() configures the pins (input +
 * pull-up), and capturePad() writes CPad::PCTempJoyState directly, so
 * ControllerConfig and game logic stay untouched. Pin map follows the reference
 * handheld (lv_gba_emu rpi port); each is overridable via env.
 * Buttons are active-low (wired to GND): unpressed reads 1, pressed 0.
 *
 * Active with the SPI build (RE3_OUTPUT_SPI) and no other input source.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_OUTPUT_SPI) && !defined(RE3_INPUT_EVDEV) && !defined(RE3_INPUT_SDL)

#include "input_source.h"

#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "Pad.h"
#include "pi_gpio.h"

struct GpioKey { const char *env; int pin; };
enum {
	GK_UP, GK_DOWN, GK_LEFT, GK_RIGHT,
	GK_A, GK_B, GK_SELECT, GK_START, GK_L, GK_R,
	GK_COUNT
};
static GpioKey sKeys[GK_COUNT] = {
	{ "RE3_KEY_UP",     12 },
	{ "RE3_KEY_DOWN",   20 },
	{ "RE3_KEY_LEFT",   21 },
	{ "RE3_KEY_RIGHT",  13 },
	{ "RE3_KEY_A",      23 },	// Cross  (accelerate / enter / confirm)
	{ "RE3_KEY_B",       4 },	// Circle (fire / cancel)
	{ "RE3_KEY_SELECT", 16 },	// Select (change camera)
	{ "RE3_KEY_START",  26 },	// Start  (pause / menu)
	{ "RE3_KEY_L",       5 },	// L1     (look left / target)
	{ "RE3_KEY_R",       6 }	// R1     (look right / target)
};
static bool sReady = false;

static void
gpio_init(void)
{
	if (pi_gpio_init() < 0) {
		printf("gpio: pi_gpio_init failed; input unavailable\n");
		return;
	}
	for (int i = 0; i < GK_COUNT; i++) {
		const char *e = getenv(sKeys[i].env);
		if (e != 0)
			sKeys[i].pin = atoi(e);
		pi_gpio_set_mode((uint8)sKeys[i].pin, PI_GPIO_INPUT);
		pi_gpio_set_pull((uint8)sKeys[i].pin, PI_GPIO_PULL_UP);
	}
	sReady = true;
	printf("gpio: input ready (UP=%d DOWN=%d LEFT=%d RIGHT=%d A=%d B=%d SELECT=%d START=%d L=%d R=%d)\n",
		sKeys[GK_UP].pin, sKeys[GK_DOWN].pin, sKeys[GK_LEFT].pin, sKeys[GK_RIGHT].pin,
		sKeys[GK_A].pin, sKeys[GK_B].pin, sKeys[GK_SELECT].pin, sKeys[GK_START].pin,
		sKeys[GK_L].pin, sKeys[GK_R].pin);
}

static bool
pressed(int idx)
{
	return pi_gpio_get_value((uint8)sKeys[idx].pin) == 0;	// active-low
}

static void
gpio_capturePad(int padID)
{
	if (padID != 0 || !sReady)
		return;

	CPad *pad = CPad::GetPad(0);
	CControllerState &s = pad->PCTempJoyState;
	s.Clear();

	bool up = pressed(GK_UP), down = pressed(GK_DOWN);
	bool left = pressed(GK_LEFT), right = pressed(GK_RIGHT);

	// D-pad (menu nav) + left stick (in-game movement); game applies deadzone.
	s.DPadUp    = up    ? 255 : 0;
	s.DPadDown  = down  ? 255 : 0;
	s.DPadLeft  = left  ? 255 : 0;
	s.DPadRight = right ? 255 : 0;
	s.LeftStickX = (int16)((right ? 128 : 0) - (left ? 128 : 0));
	s.LeftStickY = (int16)((down  ? 128 : 0) - (up   ? 128 : 0));

	s.Cross    = pressed(GK_A)      ? 255 : 0;
	s.Circle   = pressed(GK_B)      ? 255 : 0;
	s.Select   = pressed(GK_SELECT) ? 255 : 0;
	s.Start    = pressed(GK_START)  ? 255 : 0;
	s.LeftShoulder1  = pressed(GK_L) ? 255 : 0;
	s.RightShoulder1 = pressed(GK_R) ? 255 : 0;
}

static void gpio_poll(void) {}
static void gpio_terminate(void) {}

static InputSource sSrc = { gpio_init, gpio_poll, gpio_terminate, gpio_capturePad, "gpio" };

InputSource *InputSource_Get(void) { return &sSrc; }

#endif
