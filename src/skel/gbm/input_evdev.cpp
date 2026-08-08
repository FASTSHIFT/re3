/*
 * input_evdev.cpp - InputSource reading keyboard, mouse and gamepad via evdev.
 *
 * Opens /dev/input/event* directly (no SDL/udev) and classifies each device as
 * keyboard, relative mouse or gamepad:
 *   - keyboard -> RsKeyboardEventHandler (rsKEYDOWN/rsKEYUP)
 *   - mouse    -> GbmFeedMouse (absolute pos accumulated from REL_X/Y)
 *   - gamepad  -> CPad::GetPad(0)->PCTempJoyState via capturePad()
 *
 * Hotplug: a single inotify watch on /dev/input picks up devices added or
 * removed at runtime (e.g. a controller connected after launch). Nodes that
 * error out on read (unplugged) are dropped from the table too. This runs
 * inside poll(), no extra thread.
 *
 * Gamepad mapping is based on the kernel's standard game controller layout
 * (BTN_SOUTH/EAST/..., ABS_X/Y/RX/RY, ABS_Z/RZ triggers, ABS_HAT0X/Y dpad),
 * which is what hid-playstation exposes for a DualSense. Stick axes are
 * 0..255 with ~128 centre; we normalise to re3's +/-128 stick range.
 *
 * Active when RE3_INPUT_EVDEV is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_INPUT_EVDEV)

#include "input_source.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <linux/input.h>

#include "common.h"
#include "skeleton.h"
#include "crossplatform.h" // GLFW_MOUSE_BUTTON_* constants
#include "Pad.h"
#include "Frontend.h" // FrontEndMenuManager.m_bMenuActive

// ---- bit helpers ----------------------------------------------------------
#define BITS_PER_LONG (8 * sizeof(long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static inline bool
test_bit(const unsigned long *arr, int bit)
{
	return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

// ---- device table ---------------------------------------------------------
enum DevKind { DK_KEYBOARD, DK_MOUSE, DK_GAMEPAD };

#define EVDEV_MAX_DEVS 16
struct EvDev {
	int fd;
	DevKind kind;
	char node[32]; // e.g. "event2", used to match inotify delete events
};
static EvDev sDevs[EVDEV_MAX_DEVS];
static int sNumDevs = 0;

static int sKeymap[KEY_MAX + 1]; // KEY_* -> RsKeyCodes (0 = unmapped)

// Absolute mouse position (screen pixels) and button/wheel state.
static double sMouseX = 0, sMouseY = 0;
static int sMouseButtons = 0;
static bool sHaveMouse = false;

// inotify watch on /dev/input for hotplug.
static int sInotifyFd = -1;
static int sInotifyWatch = -1;

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

// ---- device classification ------------------------------------------------
// A gamepad has EV_KEY with BTN_GAMEPAD (BTN_SOUTH) and EV_ABS with ABS_X/Y.
// Checked before keyboard/mouse so a controller is never mis-classified.
static bool
is_gamepad(int fd)
{
	unsigned long evbits[NLONGS(EV_MAX)];
	memset(evbits, 0, sizeof(evbits));
	if(ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
	if(!test_bit(evbits, EV_KEY) || !test_bit(evbits, EV_ABS)) return false;

	unsigned long keybits[NLONGS(KEY_MAX)];
	memset(keybits, 0, sizeof(keybits));
	if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;
	if(!test_bit(keybits, BTN_GAMEPAD)) return false; // BTN_SOUTH

	unsigned long absbits[NLONGS(ABS_MAX)];
	memset(absbits, 0, sizeof(absbits));
	if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) return false;
	return test_bit(absbits, ABS_X) && test_bit(absbits, ABS_Y);
}

static bool
is_keyboard(int fd)
{
	unsigned long evbits[NLONGS(EV_MAX)];
	memset(evbits, 0, sizeof(evbits));
	if(ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
	if(!test_bit(evbits, EV_KEY)) return false;

	unsigned long keybits[NLONGS(KEY_MAX)];
	memset(keybits, 0, sizeof(keybits));
	if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) return false;
	int have = 0;
	int probe[] = {KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER, KEY_UP};
	for(int i = 0; i < (int)ARRAY_SIZE(probe); i++)
		if(test_bit(keybits, probe[i])) have++;
	return have >= 3;
}

// A relative pointer: EV_REL with REL_X/REL_Y and a left button.
static bool
is_mouse(int fd)
{
	unsigned long evbits[NLONGS(EV_MAX)];
	memset(evbits, 0, sizeof(evbits));
	if(ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
	if(!test_bit(evbits, EV_REL)) return false;

	unsigned long relbits[NLONGS(REL_MAX)];
	memset(relbits, 0, sizeof(relbits));
	if(ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0) return false;
	if(!test_bit(relbits, REL_X) || !test_bit(relbits, REL_Y)) return false;

	unsigned long keybits[NLONGS(KEY_MAX)];
	memset(keybits, 0, sizeof(keybits));
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	return test_bit(keybits, BTN_LEFT);
}

// Open one /dev/input/eventN, classify it, and add to the table. Returns true
// if the node was adopted. 'node' is the bare name ("eventN").
static bool
try_add_device(const char *node)
{
	if(sNumDevs >= EVDEV_MAX_DEVS) return false;
	// Already open?
	for(int i = 0; i < sNumDevs; i++)
		if(strcmp(sDevs[i].node, node) == 0) return false;

	char path[280];
	snprintf(path, sizeof(path), "/dev/input/%s", node);
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if(fd < 0) return false;

	char nm[128] = "?";
	ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);

	DevKind kind;
	// Order matters: gamepad first (a pad also has EV_KEY buttons and its
	// touchpad child exposes REL, which we do NOT want treated as a mouse).
	if(is_gamepad(fd)) {
		kind = DK_GAMEPAD;
	} else if(is_keyboard(fd)) {
		kind = DK_KEYBOARD;
	} else if(is_mouse(fd)) {
		kind = DK_MOUSE;
		sHaveMouse = true;
	} else {
		close(fd);
		return false;
	}

	EvDev *d = &sDevs[sNumDevs++];
	d->fd = fd;
	d->kind = kind;
	snprintf(d->node, sizeof(d->node), "%s", node);
	const char *kn = kind == DK_GAMEPAD ? "gamepad" : kind == DK_KEYBOARD ? "keyboard" : "mouse";
	printf("evdev: +%s %s (%s)\n", kn, path, nm);
	return true;
}

static void
remove_device_at(int idx)
{
	if(idx < 0 || idx >= sNumDevs) return;
	printf("evdev: -%s (%s)\n", sDevs[idx].node, sDevs[idx].kind == DK_GAMEPAD ? "gamepad" : sDevs[idx].kind == DK_KEYBOARD ? "keyboard" : "mouse");
	close(sDevs[idx].fd);
	// Compact the array.
	for(int i = idx; i < sNumDevs - 1; i++) sDevs[i] = sDevs[i + 1];
	sNumDevs--;
}

static void
scan_all_devices(void)
{
	DIR *dir = opendir("/dev/input");
	if(dir == 0) {
		printf("evdev: cannot open /dev/input\n");
		return;
	}
	struct dirent *de;
	while((de = readdir(dir)) != 0) {
		if(strncmp(de->d_name, "event", 5) != 0) continue;
		try_add_device(de->d_name);
	}
	closedir(dir);
}

// ---- lifecycle ------------------------------------------------------------
static void
evdev_init(void)
{
	build_keymap();
	sNumDevs = 0;
	sHaveMouse = false;

	scan_all_devices();

	// Hotplug watch: created/deleted device nodes in /dev/input.
	sInotifyFd = inotify_init1(IN_NONBLOCK);
	if(sInotifyFd >= 0) {
		sInotifyWatch = inotify_add_watch(sInotifyFd, "/dev/input", IN_CREATE | IN_DELETE);
		if(sInotifyWatch < 0) printf("evdev: inotify watch failed (hotplug disabled)\n");
	} else {
		printf("evdev: inotify_init failed (hotplug disabled)\n");
	}

	// Start the pointer near screen centre so the frontend cursor is visible.
	sMouseX = RsGlobal.maximumWidth * 0.5;
	sMouseY = RsGlobal.maximumHeight * 0.5;

	int nkb = 0, nms = 0, ngp = 0;
	for(int i = 0; i < sNumDevs; i++) nkb += sDevs[i].kind == DK_KEYBOARD, nms += sDevs[i].kind == DK_MOUSE, ngp += sDevs[i].kind == DK_GAMEPAD;
	printf("evdev: %d keyboard(s), %d mouse(s), %d gamepad(s)\n", nkb, nms, ngp);
	if(sNumDevs == 0) printf("evdev: nothing in /dev/input (need read perm; 'input' group)\n");
}

// Drain inotify and add/remove devices. Small newly-created nodes sometimes
// need a moment before EVIOCG* works; a failed try_add_device just means we
// skip it (it will not appear again via inotify, but a controller that fires
// CREATE is normally ready by the time we read it).
static void
process_hotplug(void)
{
	if(sInotifyFd < 0) return;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for(;;) {
		ssize_t n = read(sInotifyFd, buf, sizeof(buf));
		if(n <= 0) break;
		for(char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if(ev->len > 0 && strncmp(ev->name, "event", 5) == 0) {
				if(ev->mask & IN_CREATE) {
					// A device can re-enumerate under the SAME node name (e.g.
					// a DualSense that briefly drops the USB link). Drop any
					// stale entry for this node first so try_add_device re-opens
					// the fresh fd instead of skipping it as a duplicate.
					for(int i = 0; i < sNumDevs; i++)
						if(strcmp(sDevs[i].node, ev->name) == 0) {
							remove_device_at(i);
							break;
						}
					try_add_device(ev->name);
				} else if(ev->mask & IN_DELETE) {
					for(int i = 0; i < sNumDevs; i++)
						if(strcmp(sDevs[i].node, ev->name) == 0) {
							remove_device_at(i);
							break;
						}
				}
			}
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
}

// True only for errnos that mean the device is really gone, so we don't drop a
// live device on a transient read hiccup (EAGAIN means "no data", not unplug).
static inline bool
errno_is_unplug(int e)
{
	return e == ENODEV || e == EIO || e == ENXIO || e == EBADF;
}

static void
poll_keyboard(int idx)
{
	struct input_event ev[64];
	for(;;) {
		ssize_t n = read(sDevs[idx].fd, ev, sizeof(ev));
		if(n == 0) break;
		if(n < 0) {
			if(errno_is_unplug(errno)) remove_device_at(idx); // unplugged
			return;
		}
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

static void
poll_mouse(int idx, int *wheel)
{
	struct input_event ev[64];
	for(;;) {
		ssize_t n = read(sDevs[idx].fd, ev, sizeof(ev));
		if(n == 0) break;
		if(n < 0) {
			if(errno_is_unplug(errno)) remove_device_at(idx);
			return;
		}
		int count = (int)(n / sizeof(struct input_event));
		for(int j = 0; j < count; j++) {
			if(ev[j].type == EV_REL) {
				if(ev[j].code == REL_X)
					sMouseX += ev[j].value;
				else if(ev[j].code == REL_Y)
					sMouseY += ev[j].value;
				else if(ev[j].code == REL_WHEEL)
					*wheel = ev[j].value;
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

// Drain a gamepad node so its kernel buffer does not fill; the actual state is
// sampled synchronously in capturePad via EVIOCGABS/EVIOCGKEY, so here we only
// need to detect unplug (read error) and keep the fd empty.
static void
poll_gamepad(int idx)
{
	struct input_event ev[64];
	for(;;) {
		ssize_t n = read(sDevs[idx].fd, ev, sizeof(ev));
		if(n == 0) break;
		if(n < 0) {
			if(errno_is_unplug(errno)) remove_device_at(idx);
			return;
		}
	}
}

// Belt-and-suspenders hotplug: inotify gives fast add/remove, but a device can
// silently re-enumerate (USB re-bind) without a clean node delete/create pair,
// leaving us with a dead fd and no event. Every ~1s we (a) drop any tracked fd
// whose device has vanished (EVIOCGID -> ENODEV) and (b) rescan /dev/input for
// nodes we're not tracking. Both are cheap and self-correcting.
static void
health_check_and_rescan(void)
{
	for(int i = sNumDevs - 1; i >= 0; i--) {
		struct input_id id;
		if(ioctl(sDevs[i].fd, EVIOCGID, &id) < 0) remove_device_at(i);
	}
	scan_all_devices();
}

static void
evdev_poll(void)
{
	process_hotplug();

	// Throttled safety-net rescan (~1s at 30-60fps).
	static int sRescanTick = 0;
	if(++sRescanTick >= 45) {
		sRescanTick = 0;
		health_check_and_rescan();
	}

	int wheel = 0;
	// Iterate backwards so remove_device_at (which compacts) is index-safe.
	for(int i = sNumDevs - 1; i >= 0; i--) {
		switch(sDevs[i].kind) {
		case DK_KEYBOARD: poll_keyboard(i); break;
		case DK_MOUSE: poll_mouse(i, &wheel); break;
		case DK_GAMEPAD: poll_gamepad(i); break;
		}
	}

	if(sMouseX < 0) sMouseX = 0;
	if(sMouseY < 0) sMouseY = 0;
	if(sMouseX > RsGlobal.maximumWidth) sMouseX = RsGlobal.maximumWidth;
	if(sMouseY > RsGlobal.maximumHeight) sMouseY = RsGlobal.maximumHeight;
	if(sHaveMouse) GbmFeedMouse(sMouseX, sMouseY, sMouseButtons, wheel, true);
}

// ---- gamepad state sampling -----------------------------------------------
// Normalise a 0..255 stick axis (centre ~128) to re3's signed range. re3 packs
// the stick into an int16 where the effective play range is +/-128 (see the
// GLFW skeleton which writes value*128). Deadzone removes centre jitter.
static int16
norm_stick(int raw)
{
	int v = raw - 128;              // -128..127
	if(v > -12 && v < 12) return 0; // ~9% deadzone
	if(v < -128) v = -128;
	if(v > 127) v = 127;
	return (int16)v;
}

static void
gamepad_read_state(int fd, CControllerState &s)
{
	// Buttons via EVIOCGKEY bitmap.
	unsigned long keybits[NLONGS(KEY_MAX)];
	memset(keybits, 0, sizeof(keybits));
	ioctl(fd, EVIOCGKEY(sizeof(keybits)), keybits);

	bool cross = test_bit(keybits, BTN_SOUTH);
	bool circle = test_bit(keybits, BTN_EAST);
	bool triangle = test_bit(keybits, BTN_NORTH);
	bool square = test_bit(keybits, BTN_WEST);
	bool l1 = test_bit(keybits, BTN_TL);
	bool r1 = test_bit(keybits, BTN_TR);
	bool l3 = test_bit(keybits, BTN_THUMBL);
	bool r3 = test_bit(keybits, BTN_THUMBR);
	bool create = test_bit(keybits, BTN_SELECT);
	bool options = test_bit(keybits, BTN_START);

	// Axes via EVIOCGABS.
	struct input_absinfo ai;
	int lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0, hatx = 0, haty = 0;
	if(ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) lx = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) ly = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_RX), &ai) == 0) rx = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_RY), &ai) == 0) ry = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_Z), &ai) == 0) l2 = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_RZ), &ai) == 0) r2 = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_HAT0X), &ai) == 0) hatx = ai.value;
	if(ioctl(fd, EVIOCGABS(ABS_HAT0Y), &ai) == 0) haty = ai.value;

	// Sticks.
	s.LeftStickX = norm_stick(lx);
	s.LeftStickY = norm_stick(ly);
	s.RightStickX = norm_stick(rx);
	s.RightStickY = norm_stick(ry);

	// Dpad from hat axis (-1/0/1).
	bool du = haty < 0, dd = haty > 0, dl = hatx < 0, dr = hatx > 0;

	bool inMenu = !!FrontEndMenuManager.m_bMenuActive;
	if(inMenu) {
		// Left stick also drives menu navigation via the dpad.
		if(s.LeftStickY < -40) du = true;
		if(s.LeftStickY > 40) dd = true;
		if(s.LeftStickX < -40) dl = true;
		if(s.LeftStickX > 40) dr = true;
	}
	s.DPadUp = du ? 255 : 0;
	s.DPadDown = dd ? 255 : 0;
	s.DPadLeft = dl ? 255 : 0;
	s.DPadRight = dr ? 255 : 0;

	// Face buttons (PlayStation layout maps 1:1 to re3's names).
	s.Cross = cross ? 255 : 0;
	s.Circle = circle ? 255 : 0;
	s.Triangle = triangle ? 255 : 0;
	s.Square = square ? 255 : 0;

	// Shoulders and triggers.
	s.LeftShoulder1 = l1 ? 255 : 0;
	s.RightShoulder1 = r1 ? 255 : 0;
	s.LeftShoulder2 = (int16)l2;  // analogue 0..255
	s.RightShoulder2 = (int16)r2; // analogue 0..255

	s.LeftShock = l3 ? 255 : 0;
	s.RightShock = r3 ? 255 : 0;
	s.Start = options ? 255 : 0;
	s.Select = create ? 255 : 0;
}

static void
evdev_capturePad(int padID)
{
	if(padID != 0) return;
	// Use the first connected gamepad.
	int fd = -1;
	for(int i = 0; i < sNumDevs; i++)
		if(sDevs[i].kind == DK_GAMEPAD) {
			fd = sDevs[i].fd;
			break;
		}
	if(fd < 0) return;

	// PCTempJoyState is cleared once by InputSource_CapturePadAll before any
	// source runs, so we OR our state in (write non-zero fields only).
	CPad *pad = CPad::GetPad(0);
	CControllerState &s = pad->PCTempJoyState;
	gamepad_read_state(fd, s);
}

static void
evdev_terminate(void)
{
	for(int i = 0; i < sNumDevs; i++) close(sDevs[i].fd);
	sNumDevs = 0;
	if(sInotifyFd >= 0) {
		if(sInotifyWatch >= 0) inotify_rm_watch(sInotifyFd, sInotifyWatch);
		close(sInotifyFd);
		sInotifyFd = -1;
		sInotifyWatch = -1;
	}
}

static InputSource sSrc = {evdev_init, evdev_poll, evdev_terminate, evdev_capturePad, "evdev"};

static struct EvdevAutoReg {
	EvdevAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
