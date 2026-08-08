/* absinfo.c - dump EV_ABS axis ranges and EV_KEY codes for an evdev device.
 * Usage: absinfo /dev/input/event2
 * Also live-prints events for ~6s so we can confirm button<->code mapping.
 *
 * Scratch tool for probing the DualSense layout on the Pi; not built by CMake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/input.h>

static const char *
abs_name(int c)
{
	switch(c) {
	case ABS_X: return "ABS_X (LStickX)";
	case ABS_Y: return "ABS_Y (LStickY)";
	case ABS_Z: return "ABS_Z (L2)";
	case ABS_RX: return "ABS_RX (RStickX)";
	case ABS_RY: return "ABS_RY (RStickY)";
	case ABS_RZ: return "ABS_RZ (R2)";
	case ABS_HAT0X: return "ABS_HAT0X (DpadX)";
	case ABS_HAT0Y: return "ABS_HAT0Y (DpadY)";
	default: return "ABS_?";
	}
}

static const char *
key_name(int c)
{
	switch(c) {
	case BTN_SOUTH: return "BTN_SOUTH (Cross)";
	case BTN_EAST: return "BTN_EAST (Circle)";
	case BTN_NORTH: return "BTN_NORTH (Triangle)";
	case BTN_WEST: return "BTN_WEST (Square)";
	case BTN_TL: return "BTN_TL (L1)";
	case BTN_TR: return "BTN_TR (R1)";
	case BTN_TL2: return "BTN_TL2 (L2btn)";
	case BTN_TR2: return "BTN_TR2 (R2btn)";
	case BTN_SELECT: return "BTN_SELECT (Create)";
	case BTN_START: return "BTN_START (Options)";
	case BTN_MODE: return "BTN_MODE (PS)";
	case BTN_THUMBL: return "BTN_THUMBL (L3)";
	case BTN_THUMBR: return "BTN_THUMBR (R3)";
	default: return "BTN_?";
	}
}

int
main(int argc, char **argv)
{
	if(argc < 2) {
		fprintf(stderr, "usage: %s /dev/input/eventN\n", argv[0]);
		return 1;
	}
	int fd = open(argv[1], O_RDONLY);
	if(fd < 0) {
		perror("open");
		return 1;
	}

	printf("=== ABS axis ranges ===\n");
	int axes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
	for(int i = 0; i < (int)(sizeof(axes) / sizeof(axes[0])); i++) {
		struct input_absinfo ai;
		if(ioctl(fd, EVIOCGABS(axes[i]), &ai) == 0)
			printf("  %-20s min=%d max=%d flat=%d fuzz=%d val=%d\n", abs_name(axes[i]), ai.minimum, ai.maximum, ai.flat, ai.fuzz, ai.value);
	}

	printf("=== live events (~6s, press buttons/sticks) ===\n");
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for(;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double dt = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
		if(dt > 6.0) break;
		struct input_event ev[32];
		ssize_t n = read(fd, ev, sizeof(ev));
		if(n <= 0) {
			usleep(5000);
			continue;
		}
		int cnt = (int)(n / sizeof(struct input_event));
		for(int j = 0; j < cnt; j++) {
			if(ev[j].type == EV_KEY)
				printf("  KEY  code=0x%03x %-22s val=%d\n", ev[j].code, key_name(ev[j].code), ev[j].value);
			else if(ev[j].type == EV_ABS)
				printf("  ABS  %-20s val=%d\n", abs_name(ev[j].code), ev[j].value);
		}
	}
	close(fd);
	return 0;
}
