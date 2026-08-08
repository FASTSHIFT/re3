/*
 * input_source.h - pluggable input for the headless (GBM) skeleton.
 *
 * Multiple input sources can be active simultaneously (e.g. GPIO gamepad +
 * evdev keyboard). gbm.cpp calls InputSource_InitAll/PollAll/TerminateAll
 * and CapturePad calls InputSource_CapturePadAll(). Each source injects into
 * re3 independently:
 *   - evdev : /dev/input/event* keyboard -> RsKeyboardEventHandler
 *   - gpio  : physical buttons -> CPad::PCTempJoyState (method A)
 *   - sdl   : SDL2 keyboard/mouse -> RsKeyboardEventHandler
 *
 * Sources self-register at startup by calling InputSource_Register().
 * See docs/08 section 5.
 */
#ifndef RE3_INPUT_SOURCE_H
#define RE3_INPUT_SOURCE_H

#define INPUT_SOURCE_MAX 4

struct InputSource {
	void (*init)(void);
	void (*poll)(void);
	void (*terminate)(void);
	void (*capturePad)(int padID); // null if not a gamepad source
	const char *name;
};

// Register a source. Called from each source's file-scope constructor / init.
bool
InputSource_Register(InputSource *src);

// Lifecycle: called from gbm.cpp once at startup/shutdown, and each frame.
void
InputSource_InitAll(void);
void
InputSource_PollAll(void);
void
InputSource_TerminateAll(void);

// Called from CapturePad each frame to let gamepad sources write PCTempJoyState.
void
InputSource_CapturePadAll(int padID);

// Compatibility shim: returns the first registered source (SDL single-source builds).
InputSource *
InputSource_Get(void);

// Shared mouse feed (implemented in gbm.cpp, used by SDL/evdev sources).
void
GbmFeedMouse(double x, double y, int buttons, int wheel, bool inWindow);

#endif /* RE3_INPUT_SOURCE_H */
