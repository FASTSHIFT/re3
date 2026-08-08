/*
 * input_evdev.cpp - InputSource reading a keyboard via evdev (/dev/input).
 * ...
 * Active when RE3_INPUT_EVDEV is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_INPUT_EVDEV)

#include "input_source.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "common.h"
#include "skeleton.h"
#include "crossplatform.h" // GLFW_MOUSE_BUTTON_* constants

#define EVDEV_MAX_FDS 8
static int sFds[EVDEV_MAX_FDS]; // keyboard fds
static int sNumFds = 0;
static int sMouseFds[EVDEV_MAX_FDS]; // mouse/pointer fds
static int sNumMouseFds = 0;
static int sKeymap[KEY_MAX + 1]; // KEY_* -> RsKeyCodes (0 = unmapped)

// Accumulated absolute mouse position (render/screen pixels) and button state.
static double sMouseX = 0, sMouseY = 0;
static int sMouseButtons = 0;

static void
build_keymap(void)
{
	for(int i = 0; i <= KEY_MAX; i++) sKeymap[i] = 0;

	sKeymap[KEY_A] = 'A';
	sKeymap[KEY_B] = 'B';
	sKeymap[KEY_C] = 'C';
	sKeymap[KEY_D] = 'D';
	sKeymap[KEY_E] = 'E';
	sKeymap[KEY_F] = 'F';
	sKeymap[KEY_G] = 'G';
	sKeymap[KEY_H] = 'H';
	sKeymap[KEY_I] = 'I';
	sKeymap[KEY_J] = 'J';
	sKeymap[KEY_K] = 'K';
	sKeymap[KEY_L] = 'L';
	sKeymap[KEY_M] = 'M';
	sKeymap[KEY_N] = 'N';
	sKeymap[KEY_O] = 'O';
	sKeymap[KEY_P] = 'P';
	sKeymap[KEY_Q] = 'Q';
	sKeymap[KEY_R] = 'R';
	sKeymap[KEY_S] = 'S';
	sKeymap[KEY_T] = 'T';
	sKeymap[KEY_U] = 'U';
	sKeymap[KEY_V] = 'V';
	sKeymap[KEY_W] = 'W';
	sKeymap[KEY_X] = 'X';
	sKeymap[KEY_Y] = 'Y';
	sKeymap[KEY_Z] = 'Z';

	sKeymap[KEY_0] = '0';
	sKeymap[KEY_1] = '1';
	sKeymap[KEY_2] = '2';
	sKeymap[KEY_3] = '3';
	sKeymap[KEY_4] = '4';
	sKeymap[KEY_5] = '5';
	sKeymap[KEY_6] = '6';
	sKeymap[KEY_7] = '7';
	sKeymap[KEY_8] = '8';
	sKeymap[KEY_9] = '9';

	sKeymap[KEY_SPACE] = ' ';
	sKeymap[KEY_APOSTROPHE] = '\'';
	sKeymap[KEY_COMMA] = ',';
	sKeymap[KEY_MINUS] = '-';
	sKeymap[KEY_DOT] = '.';
	sKeymap[KEY_SLASH] = '/';
	sKeymap[KEY_SEMICOLON] = ';';
	sKeymap[KEY_EQUAL] = '=';
	sKeymap[KEY_LEFTBRACE] = '[';
	sKeymap[KEY_BACKSLASH] = '\\';
	sKeymap[KEY_RIGHTBRACE] = ']';
	sKeymap[KEY_GRAVE] = '`';

	sKeymap[KEY_ESC] = rsESC;
	sKeymap[KEY_ENTER] = rsENTER;
	sKeymap[KEY_TAB] = rsTAB;
	sKeymap[KEY_BACKSPACE] = rsBACKSP;
	sKeymap[KEY_INSERT] = rsINS;
	sKeymap[KEY_DELETE] = rsDEL;
	sKeymap[KEY_RIGHT] = rsRIGHT;
	sKeymap[KEY_LEFT] = rsLEFT;
	sKeymap[KEY_DOWN] = rsDOWN;
	sKeymap[KEY_UP] = rsUP;
	sKeymap[KEY_PAGEUP] = rsPGUP;
	sKeymap[KEY_PAGEDOWN] = rsPGDN;
	sKeymap[KEY_HOME] = rsHOME;
	sKeymap[KEY_END] = rsEND;
	sKeymap[KEY_CAPSLOCK] = rsCAPSLK;
	sKeymap[KEY_SCROLLLOCK] = rsSCROLL;
	sKeymap[KEY_NUMLOCK] = rsNUMLOCK;
	sKeymap[KEY_PAUSE] = rsPAUSE;

	sKeymap[KEY_F1] = rsF1;
	sKeymap[KEY_F2] = rsF2;
	sKeymap[KEY_F3] = rsF3;
	sKeymap[KEY_F4] = rsF4;
	sKeymap[KEY_F5] = rsF5;
	sKeymap[KEY_F6] = rsF6;
	sKeymap[KEY_F7] = rsF7;
	sKeymap[KEY_F8] = rsF8;
	sKeymap[KEY_F9] = rsF9;
	sKeymap[KEY_F10] = rsF10;
	sKeymap[KEY_F11] = rsF11;
	sKeymap[KEY_F12] = rsF12;

	sKeymap[KEY_KP0] = rsPADINS;
	sKeymap[KEY_KP1] = rsPADEND;
	sKeymap[KEY_KP2] = rsPADDOWN;
	sKeymap[KEY_KP3] = rsPADPGDN;
	sKeymap[KEY_KP4] = rsPADLEFT;
	sKeymap[KEY_KP5] = rsPAD5;
	sKeymap[KEY_KP6] = rsPADRIGHT;
	sKeymap[KEY_KP7] = rsPADHOME;
	sKeymap[KEY_KP8] = rsPADUP;
	sKeymap[KEY_KP9] = rsPADPGUP;
	sKeymap[KEY_KPDOT] = rsPADDEL;
	sKeymap[KEY_KPSLASH] = rsDIVIDE;
	sKeymap[KEY_KPASTERISK] = rsTIMES;
	sKeymap[KEY_KPMINUS] = rsMINUS;
	sKeymap[KEY_KPPLUS] = rsPLUS;
	sKeymap[KEY_KPENTER] = rsPADENTER;

	sKeymap[KEY_LEFTSHIFT] = rsLSHIFT;
	sKeymap[KEY_RIGHTSHIFT] = rsRSHIFT;
	sKeymap[KEY_LEFTCTRL] = rsLCTRL;
	sKeymap[KEY_RIGHTCTRL] = rsRCTRL;
	sKeymap[KEY_LEFTALT] = rsLALT;
	sKeymap[KEY_RIGHTALT] = rsRALT;
	sKeymap[KEY_LEFTMETA] = rsLWIN;
	sKeymap[KEY_RIGHTMETA] = rsRWIN;
}

static bool
is_keyboard(int fd)
{
	unsigned long evbits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(evbits, 0, sizeof(evbits));
	if(ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
	if(!(evbits[EV_KEY / (8 * sizeof(long))] & (1UL << (EV_KEY % (8 * sizeof(long)))))) return false;

	unsigned long keybits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(keybits, 0, sizeof(keybits));
	if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;
	int have = 0;
	int probe[] = {KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER, KEY_UP};
	for(int i = 0; i < (int)ARRAY_SIZE(probe); i++) {
		int k = probe[i];
		if(keybits[k / (8 * sizeof(long))] & (1UL << (k % (8 * sizeof(long))))) have++;
	}
	return have >= 3;
}

// A relative pointer: has EV_REL with REL_X/REL_Y and a mouse button.
static bool
is_mouse(int fd)
{
	unsigned long evbits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(evbits, 0, sizeof(evbits));
	if(ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
	if(!(evbits[EV_REL / (8 * sizeof(long))] & (1UL << (EV_REL % (8 * sizeof(long)))))) return false;

	unsigned long relbits[(REL_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(relbits, 0, sizeof(relbits));
	if(ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0) return false;
	bool hasX = relbits[REL_X / (8 * sizeof(long))] & (1UL << (REL_X % (8 * sizeof(long))));
	bool hasY = relbits[REL_Y / (8 * sizeof(long))] & (1UL << (REL_Y % (8 * sizeof(long))));

	unsigned long keybits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	memset(keybits, 0, sizeof(keybits));
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	bool hasBtn = keybits[BTN_LEFT / (8 * sizeof(long))] & (1UL << (BTN_LEFT % (8 * sizeof(long))));

	return hasX && hasY && hasBtn;
}

static void
evdev_init(void)
{
	build_keymap();
	sNumFds = 0;
	sNumMouseFds = 0;

	DIR *dir = opendir("/dev/input");
	if(dir == 0) {
		printf("evdev: cannot open /dev/input\n");
		return;
	}
	struct dirent *de;
	while((de = readdir(dir)) != 0) {
		if(strncmp(de->d_name, "event", 5) != 0) continue;
		char path[280];
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if(fd < 0) continue;
		char nm[128] = "?";
		ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
		if(is_keyboard(fd) && sNumFds < EVDEV_MAX_FDS) {
			printf("evdev: keyboard %s (%s)\n", path, nm);
			sFds[sNumFds++] = fd;
		} else if(is_mouse(fd) && sNumMouseFds < EVDEV_MAX_FDS) {
			printf("evdev: mouse %s (%s)\n", path, nm);
			sMouseFds[sNumMouseFds++] = fd;
		} else {
			close(fd);
		}
	}
	closedir(dir);

	// Start the pointer near screen centre so the frontend cursor is visible.
	sMouseX = RsGlobal.maximumWidth * 0.5;
	sMouseY = RsGlobal.maximumHeight * 0.5;

	if(sNumFds == 0) printf("evdev: no keyboard in /dev/input (need read perm; 'input' group)\n");
	if(sNumMouseFds == 0) printf("evdev: no mouse in /dev/input\n");
}

static void
evdev_poll(void)
{
	struct input_event ev[64];

	// Keyboard.
	for(int i = 0; i < sNumFds; i++) {
		for(;;) {
			ssize_t n = read(sFds[i], ev, sizeof(ev));
			if(n <= 0) break;
			int count = (int)(n / sizeof(struct input_event));
			for(int j = 0; j < count; j++) {
				if(ev[j].type != EV_KEY || ev[j].code > KEY_MAX) continue;
				int rs = sKeymap[ev[j].code];
				if(rs == 0) continue;
				if(ev[j].value == 1)
					RsKeyboardEventHandler(rsKEYDOWN, &rs);
				else if(ev[j].value == 0)
					RsKeyboardEventHandler(rsKEYUP, &rs);
			}
		}
	}

	// Mouse: accumulate relative motion into an absolute position (clamped to
	// the screen), track buttons and wheel, then feed re3.
	int wheel = 0;
	for(int i = 0; i < sNumMouseFds; i++) {
		for(;;) {
			ssize_t n = read(sMouseFds[i], ev, sizeof(ev));
			if(n <= 0) break;
			int count = (int)(n / sizeof(struct input_event));
			for(int j = 0; j < count; j++) {
				if(ev[j].type == EV_REL) {
					if(ev[j].code == REL_X)
						sMouseX += ev[j].value;
					else if(ev[j].code == REL_Y)
						sMouseY += ev[j].value;
					else if(ev[j].code == REL_WHEEL)
						wheel = ev[j].value;
				} else if(ev[j].type == EV_KEY) {
					int bit = -1;
					switch(ev[j].code) {
					case BTN_LEFT: bit = GLFW_MOUSE_BUTTON_LEFT; break;
					case BTN_RIGHT: bit = GLFW_MOUSE_BUTTON_RIGHT; break;
					case BTN_MIDDLE: bit = GLFW_MOUSE_BUTTON_MIDDLE; break;
					case BTN_SIDE: bit = GLFW_MOUSE_BUTTON_4; break;
					case BTN_EXTRA: bit = GLFW_MOUSE_BUTTON_5; break;
					default: break;
					}
					if(bit >= 0) {
						if(ev[j].value)
							sMouseButtons |= (1 << bit);
						else
							sMouseButtons &= ~(1 << bit);
					}
				}
			}
		}
	}
	if(sMouseX < 0) sMouseX = 0;
	if(sMouseY < 0) sMouseY = 0;
	if(sMouseX > RsGlobal.maximumWidth) sMouseX = RsGlobal.maximumWidth;
	if(sMouseY > RsGlobal.maximumHeight) sMouseY = RsGlobal.maximumHeight;

	if(sNumMouseFds > 0) GbmFeedMouse(sMouseX, sMouseY, sMouseButtons, wheel, true);
}

static void
evdev_terminate(void)
{
	for(int i = 0; i < sNumFds; i++) close(sFds[i]);
	for(int i = 0; i < sNumMouseFds; i++) close(sMouseFds[i]);
	sNumFds = 0;
	sNumMouseFds = 0;
}

static InputSource sSrc = {evdev_init, evdev_poll, evdev_terminate, 0, "evdev"};

static struct EvdevAutoReg {
	EvdevAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
