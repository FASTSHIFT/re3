/*
 * input_sdl.cpp - InputSource reading the keyboard via SDL2 (desktop debug).
 *
 * Translates SDL scancodes to re3 RsKeyCodes and feeds RsKeyboardEventHandler.
 * Paired with sink_sdl.cpp for the desktop/emulator debug build. SDL video is
 * initialized by the sink; this source only needs the event queue, so it works
 * as long as SDL_INIT_VIDEO was set up (the sink does it before first poll).
 *
 * Active when RE3_INPUT_SDL is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_INPUT_SDL)

#include "input_source.h"

#include <SDL2/SDL.h>

#include "common.h"
#include "skeleton.h"
#include "crossplatform.h"	// gGbmMouse* shim state
#include "platform.h"		// PSGLOBAL
#include "Frontend.h"		// FrontEndMenuManager.m_bMenuActive

static int sKeymap[SDL_NUM_SCANCODES];
bool gInGameMouseCapture = false;	// SDL relative-mouse state (in-game look)

static void
build_keymap(void)
{
	for (int i = 0; i < SDL_NUM_SCANCODES; i++)
		sKeymap[i] = 0;

	for (int c = 0; c < 26; c++) sKeymap[SDL_SCANCODE_A + c] = 'A' + c;
	sKeymap[SDL_SCANCODE_1]='1'; sKeymap[SDL_SCANCODE_2]='2'; sKeymap[SDL_SCANCODE_3]='3';
	sKeymap[SDL_SCANCODE_4]='4'; sKeymap[SDL_SCANCODE_5]='5'; sKeymap[SDL_SCANCODE_6]='6';
	sKeymap[SDL_SCANCODE_7]='7'; sKeymap[SDL_SCANCODE_8]='8'; sKeymap[SDL_SCANCODE_9]='9';
	sKeymap[SDL_SCANCODE_0]='0';

	sKeymap[SDL_SCANCODE_SPACE]=' '; sKeymap[SDL_SCANCODE_APOSTROPHE]='\'';
	sKeymap[SDL_SCANCODE_COMMA]=','; sKeymap[SDL_SCANCODE_MINUS]='-';
	sKeymap[SDL_SCANCODE_PERIOD]='.'; sKeymap[SDL_SCANCODE_SLASH]='/';
	sKeymap[SDL_SCANCODE_SEMICOLON]=';'; sKeymap[SDL_SCANCODE_EQUALS]='=';
	sKeymap[SDL_SCANCODE_LEFTBRACKET]='['; sKeymap[SDL_SCANCODE_BACKSLASH]='\\';
	sKeymap[SDL_SCANCODE_RIGHTBRACKET]=']'; sKeymap[SDL_SCANCODE_GRAVE]='`';

	sKeymap[SDL_SCANCODE_ESCAPE]=rsESC; sKeymap[SDL_SCANCODE_RETURN]=rsENTER;
	sKeymap[SDL_SCANCODE_TAB]=rsTAB; sKeymap[SDL_SCANCODE_BACKSPACE]=rsBACKSP;
	sKeymap[SDL_SCANCODE_INSERT]=rsINS; sKeymap[SDL_SCANCODE_DELETE]=rsDEL;
	sKeymap[SDL_SCANCODE_RIGHT]=rsRIGHT; sKeymap[SDL_SCANCODE_LEFT]=rsLEFT;
	sKeymap[SDL_SCANCODE_DOWN]=rsDOWN; sKeymap[SDL_SCANCODE_UP]=rsUP;
	sKeymap[SDL_SCANCODE_PAGEUP]=rsPGUP; sKeymap[SDL_SCANCODE_PAGEDOWN]=rsPGDN;
	sKeymap[SDL_SCANCODE_HOME]=rsHOME; sKeymap[SDL_SCANCODE_END]=rsEND;
	sKeymap[SDL_SCANCODE_CAPSLOCK]=rsCAPSLK; sKeymap[SDL_SCANCODE_PAUSE]=rsPAUSE;

	sKeymap[SDL_SCANCODE_F1]=rsF1; sKeymap[SDL_SCANCODE_F2]=rsF2; sKeymap[SDL_SCANCODE_F3]=rsF3;
	sKeymap[SDL_SCANCODE_F4]=rsF4; sKeymap[SDL_SCANCODE_F5]=rsF5; sKeymap[SDL_SCANCODE_F6]=rsF6;
	sKeymap[SDL_SCANCODE_F7]=rsF7; sKeymap[SDL_SCANCODE_F8]=rsF8; sKeymap[SDL_SCANCODE_F9]=rsF9;
	sKeymap[SDL_SCANCODE_F10]=rsF10; sKeymap[SDL_SCANCODE_F11]=rsF11; sKeymap[SDL_SCANCODE_F12]=rsF12;

	sKeymap[SDL_SCANCODE_LSHIFT]=rsLSHIFT; sKeymap[SDL_SCANCODE_RSHIFT]=rsRSHIFT;
	sKeymap[SDL_SCANCODE_LCTRL]=rsLCTRL; sKeymap[SDL_SCANCODE_RCTRL]=rsRCTRL;
	sKeymap[SDL_SCANCODE_LALT]=rsLALT; sKeymap[SDL_SCANCODE_RALT]=rsRALT;
}

static void
sdlin_init(void)
{
	// SDL_INIT_VIDEO (done by the SDL sink) also enables the event queue.
	if (!SDL_WasInit(SDL_INIT_VIDEO))
		SDL_InitSubSystem(SDL_INIT_VIDEO);
	build_keymap();
	printf("input: SDL keyboard + mouse\n");
}

static void
sdlin_poll(void)
{
	SDL_PumpEvents();

	// Keyboard: peek key events only (the sink drains SDL_QUIT separately).
	SDL_Event evs[64];
	int n = SDL_PeepEvents(evs, 64, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYUP);
	for (int i = 0; i < n; i++) {
		SDL_Scancode sc = evs[i].key.keysym.scancode;
		if (sc >= SDL_NUM_SCANCODES)
			continue;
		int rs = sKeymap[sc];
		if (rs == 0)
			continue;
		if (evs[i].type == SDL_KEYDOWN) {
			if (evs[i].key.repeat == 0)
				RsKeyboardEventHandler(rsKEYDOWN, &rs);
		} else {
			RsKeyboardEventHandler(rsKEYUP, &rs);
		}
	}

	// Mouse: in menus use absolute window coords; in-game use SDL relative-mouse
	// mode for unbounded camera look (matches the original GLFW_CURSOR_DISABLED).
	bool wantRelative = !FrontEndMenuManager.m_bMenuActive;
	if (wantRelative != gInGameMouseCapture) {
		SDL_SetRelativeMouseMode(wantRelative ? SDL_TRUE : SDL_FALSE);
		gInGameMouseCapture = wantRelative;
	}

	// Drain motion/button/wheel events (the sink only takes SDL_QUIT, so these
	// are ours). Accumulate relative motion from xrel/yrel, which works in both
	// relative and absolute modes and doesn't depend on window-grab timing.
	static double px = 0, py = 0;
	int wheel = 0;
	SDL_Event mev[64];
	int mn = SDL_PeepEvents(mev, 64, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
	for (int i = 0; i < mn; i++) {
		if (mev[i].type == SDL_MOUSEMOTION) {
			if (wantRelative) {
				px += mev[i].motion.xrel;
				py += mev[i].motion.yrel;
			} else {
				px = mev[i].motion.x;
				py = mev[i].motion.y;
			}
		} else if (mev[i].type == SDL_MOUSEWHEEL) {
			wheel = mev[i].wheel.y;
		}
	}

	Uint32 btn = SDL_GetMouseState(0, 0);
	int buttons =
		((btn & SDL_BUTTON(SDL_BUTTON_LEFT))   ? (1 << GLFW_MOUSE_BUTTON_LEFT)   : 0) |
		((btn & SDL_BUTTON(SDL_BUTTON_RIGHT))  ? (1 << GLFW_MOUSE_BUTTON_RIGHT)  : 0) |
		((btn & SDL_BUTTON(SDL_BUTTON_MIDDLE)) ? (1 << GLFW_MOUSE_BUTTON_MIDDLE) : 0) |
		((btn & SDL_BUTTON(SDL_BUTTON_X1))     ? (1 << GLFW_MOUSE_BUTTON_4)      : 0) |
		((btn & SDL_BUTTON(SDL_BUTTON_X2))     ? (1 << GLFW_MOUSE_BUTTON_5)      : 0);

	GbmFeedMouse(px, py, buttons, wheel, true);
}

static void
sdlin_terminate(void)
{
}

static InputSource sSrc = { sdlin_init, sdlin_poll, sdlin_terminate, 0, "sdl" };

static struct SdlInputAutoReg {
	SdlInputAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
