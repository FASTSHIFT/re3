/*
 * input_evdev.cpp - InputSource reading keyboard, mouse and gamepad via evdev.
 *
 * Opens /dev/input/event* directly (no SDL/udev) and classifies each device as
 * keyboard, relative mouse or gamepad:
 *   - keyboard -> RsKeyboardEventHandler (rsKEYDOWN/rsKEYUP)
 *   - mouse    -> GbmFeedMouse (absolute pos accumulated from REL_X/Y)
 *   - gamepad  -> CPad::GetPad(0)->PCTempJoyState via capturePad()
 *
 * Hotplug: a netlink uevent socket (the mechanism udev/SDL use underneath)
 * delivers kernel add@/remove@ events for /dev/input nodes at runtime, e.g. a
 * controller connected after launch. The add@ event arrives once the device is
 * ready (unlike inotify's IN_CREATE, which fires before the driver has probed),
 * so no ready-retry workaround is needed. Nodes that error out on read
 * (unplugged) are dropped too. Everything runs inside poll(), no extra thread.
 * See docs/10.
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
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/input.h>

#include "common.h"
#include "skeleton.h"
#include "crossplatform.h" // GLFW_MOUSE_BUTTON_* constants
#include "Pad.h"
#include "Frontend.h" // FrontEndMenuManager.m_bMenuActive
#ifdef RE3_CHEATS
#include "cheat_input.h"
#endif

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

// netlink uevent socket for hotplug (see docs/10). The kernel's add@ uevent is
// broadcast once the input device itself is ready (unlike inotify's IN_CREATE
// which fires before the driver has even probed). One subtlety remains: the
// kernel announces add@ BEFORE udev applies the node's ACL/'input' group
// permission, so the first open() can race and fail with EACCES. We therefore
// queue add@ nodes for a few frames of retries (sPending) rather than relying
// on a single immediate open; the permission lands within tens of ms.
static int sUeventFd = -1;

#define EVDEV_MAX_PENDING 8
#define EVDEV_PENDING_TRIES 60 // frames; ~1-2s window covers the udev ACL delay
struct Pending {
	char node[32];
	int tries;
};
static Pending sPending[EVDEV_MAX_PENDING];
static int sNumPending = 0;

static void
pending_add(const char *node)
{
	for(int i = 0; i < sNumPending; i++)
		if(strcmp(sPending[i].node, node) == 0) {
			sPending[i].tries = EVDEV_PENDING_TRIES; // refresh window
			return;
		}
	if(sNumPending >= EVDEV_MAX_PENDING) return;
	snprintf(sPending[sNumPending].node, sizeof(sPending[sNumPending].node), "%s", node);
	sPending[sNumPending].tries = EVDEV_PENDING_TRIES;
	sNumPending++;
}

static void
pending_remove_at(int idx)
{
	for(int i = idx; i < sNumPending - 1; i++) sPending[i] = sPending[i + 1];
	sNumPending--;
}

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
	if(fd < 0) {
		// EACCES here is expected on hotplug: the kernel broadcasts add@ before
		// udev has applied the ACL/'input' group permission to the new node, so
		// the very first open races and fails. The caller queues a short retry
		// (see pending list); by the next attempt udev has set permissions.
		return false;
	}

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
	sNumPending = 0;
	sHaveMouse = false;

#ifdef RE3_CHEATS
	CheatInput_Init();
#endif

	scan_all_devices();

	// Hotplug: subscribe to kernel uevents via netlink (group 1 = broadcast).
	sUeventFd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if(sUeventFd >= 0) {
		// group 1 carries EVERY subsystem's uevents, so a single USB hotplug of
		// a composite device (plus unrelated system activity) can burst dozens
		// of messages. Grow the receive buffer so a burst doesn't overflow the
		// socket (which would otherwise start returning ENOBUFS). Try the
		// privileged force variant first, fall back to the normal one.
		int rcvbuf = 1024 * 1024;
		if(setsockopt(sUeventFd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
			setsockopt(sUeventFd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

		struct sockaddr_nl addr;
		memset(&addr, 0, sizeof(addr));
		addr.nl_family = AF_NETLINK;
		addr.nl_pid = 0;    // let the kernel assign
		addr.nl_groups = 1; // kernel uevent broadcast group
		if(bind(sUeventFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			printf("evdev: uevent bind failed (hotplug disabled)\n");
			close(sUeventFd);
			sUeventFd = -1;
		}
	} else {
		printf("evdev: uevent socket failed (hotplug disabled)\n");
	}

	// Start the pointer near screen centre so the frontend cursor is visible.
	sMouseX = RsGlobal.maximumWidth * 0.5;
	sMouseY = RsGlobal.maximumHeight * 0.5;

	int nkb = 0, nms = 0, ngp = 0;
	for(int i = 0; i < sNumDevs; i++) nkb += sDevs[i].kind == DK_KEYBOARD, nms += sDevs[i].kind == DK_MOUSE, ngp += sDevs[i].kind == DK_GAMEPAD;
	printf("evdev: %d keyboard(s), %d mouse(s), %d gamepad(s)\n", nkb, nms, ngp);
	if(sNumDevs == 0) printf("evdev: nothing in /dev/input (need read perm; 'input' group)\n");
}

// Handle one add/remove for a bare node name ("eventN").
static void
handle_hotplug_action(const char *action, const char *node)
{
	if(strncmp(node, "event", 5) != 0) return;

	if(strcmp(action, "add") == 0) {
		// A device can re-enumerate under the SAME node name (e.g. a controller
		// that briefly drops its link). Drop any stale entry first so
		// try_add_device re-opens the fresh fd instead of skipping a duplicate.
		for(int i = 0; i < sNumDevs; i++)
			if(strcmp(sDevs[i].node, node) == 0) {
				remove_device_at(i);
				break;
			}
		// The immediate open often races udev's ACL (EACCES); if it fails,
		// queue the node for a few frames of retries.
		if(!try_add_device(node)) pending_add(node);
	} else if(strcmp(action, "remove") == 0) {
		for(int i = 0; i < sNumDevs; i++)
			if(strcmp(sDevs[i].node, node) == 0) {
				remove_device_at(i);
				break;
			}
		// Cancel any pending retry for a node that just went away.
		for(int i = 0; i < sNumPending; i++)
			if(strcmp(sPending[i].node, node) == 0) {
				pending_remove_at(i);
				break;
			}
	}
}

// Retry queued add@ nodes whose first open raced udev's ACL. Called each frame.
static void
process_pending(void)
{
	for(int i = sNumPending - 1; i >= 0; i--) {
		if(try_add_device(sPending[i].node) || --sPending[i].tries <= 0) pending_remove_at(i);
	}
}

// Drain the netlink uevent socket. Each message is a NUL-separated list of
// KEY=VALUE lines whose first token is "action@devpath". We only care about
// SUBSYSTEM=input events whose DEVNAME/devpath names an eventN node; by the
// time the kernel broadcasts add@, the node is fully ready (see docs/10).
static void
process_hotplug(void)
{
	if(sUeventFd < 0) return;
	char buf[2048];
	for(;;) {
		ssize_t n = recv(sUeventFd, buf, sizeof(buf) - 1, 0);
		if(n < 0) {
			// EAGAIN: no more messages this frame - done.
			// ENOBUFS: the socket overflowed and the kernel dropped messages;
			// the socket is still usable, so just stop for this frame and keep
			// reading next frame (do NOT treat it as fatal). Any other error we
			// also just bail on for this frame.
			break;
		}
		if(n == 0) break;
		buf[n] = '\0';

		// First line: "action@devpath" (e.g. "add@/devices/.../input/input5/event5").
		const char *action = buf;
		const char *at = strchr(buf, '@');
		if(at == 0) continue;
		size_t alen = (size_t)(at - action);

		// Parse the NUL-separated KEY=VALUE properties that follow.
		bool isInput = false;
		const char *devname = 0; // e.g. "input/event5"
		size_t off = strlen(buf) + 1;
		for(; off < (size_t)n; off += strlen(buf + off) + 1) {
			const char *line = buf + off;
			if(strncmp(line, "SUBSYSTEM=", 10) == 0)
				isInput = strcmp(line + 10, "input") == 0;
			else if(strncmp(line, "DEVNAME=", 8) == 0)
				devname = line + 8;
		}
		if(!isInput || devname == 0) continue;

		// DEVNAME is like "input/event5"; take the basename.
		const char *node = strrchr(devname, '/');
		node = node ? node + 1 : devname;

		char act[16];
		if(alen >= sizeof(act)) alen = sizeof(act) - 1;
		memcpy(act, action, alen);
		act[alen] = '\0';
		handle_hotplug_action(act, node);
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

// Cheap periodic health check: drop any tracked fd whose device has vanished
// (EVIOCGID -> error). This only touches the handful of fds we already hold
// open, costing microseconds.
//
// We deliberately do NOT rescan /dev/input here. Opening the non-input nodes
// present on a Pi (event0 vc4-hdmi, event1 HDMI Jack) and issuing EVIOCGBIT on
// them is astonishingly slow - measured ~60ms and ~196ms respectively - so a
// full rescan every second stalled the render loop for 130-256ms (the "stutter
// once a second"). inotify (process_hotplug) already delivers add/remove events
// for real hotplug, including a device that re-enumerates under the same node
// name (it fires DELETE then CREATE), so periodic rescanning buys us nothing.
static void
health_check(void)
{
	for(int i = sNumDevs - 1; i >= 0; i--) {
		struct input_id id;
		if(ioctl(sDevs[i].fd, EVIOCGID, &id) < 0) remove_device_at(i);
	}
}

static void
evdev_poll(void)
{
	process_hotplug();
	process_pending();

	// Throttled health check on tracked fds only (~1s at 30-60fps).
	static int sHealthTick = 0;
	if(++sHealthTick >= 45) {
		sHealthTick = 0;
		health_check();
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

#ifdef RE3_CHEATS
	// Gamepad cheat combos: detect on the freshly sampled state. When a combo
	// is active this frame, suppress the modifier button's normal injection so
	// e.g. triggering a cheat doesn't also fire Select. Reuses inMenu above.
	if(CheatInput_Process(s, inMenu)) {
		// Default modifier is Create -> Select; clear it. Harmless if the
		// configured modifier is a different button.
		s.Select = 0;
	}
#endif
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
	if(sUeventFd >= 0) {
		close(sUeventFd);
		sUeventFd = -1;
	}
}

static InputSource sSrc = {evdev_init, evdev_poll, evdev_terminate, evdev_capturePad, "evdev"};

static struct EvdevAutoReg {
	EvdevAutoReg() { InputSource_Register(&sSrc); }
} sAutoReg;

#endif
