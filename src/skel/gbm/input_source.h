/*
 * input_source.h - pluggable input for the headless (GBM) skeleton.
 *
 * The platform-independent layer (events.cpp -> CPad, or CapturePad ->
 * PCTempJoyState) is reused unchanged. An input source only needs to read its
 * hardware and inject into re3:
 *   - evdev : /dev/input/event* keyboard -> RsKeyboardEventHandler
 *   - gpio  : physical buttons -> CPad::PCTempJoyState (docs/08 method A)
 *   - sdl   : SDL2 keyboard events -> RsKeyboardEventHandler (desktop debug)
 *
 * poll() is called once per frame from the main loop. See docs/08 section 5.
 */
#ifndef RE3_INPUT_SOURCE_H
#define RE3_INPUT_SOURCE_H

struct InputSource
{
	void (*init)(void);
	void (*poll)(void);		// read hardware, inject into re3 (per frame)
	void (*terminate)(void);

	// Optional gamepad hook: called from CapturePad (CPad::UpdatePads path) to
	// write CPad::PCTempJoyState directly (docs/08 method A). May be null for
	// sources that inject via the keyboard event chain (evdev/sdl). 'padID' is
	// the pad index (0/1).
	void (*capturePad)(int padID);

	const char *name;
};

// Select at build time: RE3_INPUT_SDL -> sdl, RE3_INPUT_EVDEV -> evdev,
// (GPIO is coupled to the SPI build and injected via CapturePad). Returns a
// static source (never null; a no-op source if none is configured).
InputSource *InputSource_Get(void);

// Feed absolute mouse state into re3 (implemented in gbm.cpp; shared by input
// sources that have a pointer). x/y in screen pixels, buttons is a bitmask
// indexed by the GLFW_MOUSE_BUTTON_* constants, wheel is a per-frame delta.
void GbmFeedMouse(double x, double y, int buttons, int wheel, bool inWindow);

#endif /* RE3_INPUT_SOURCE_H */
