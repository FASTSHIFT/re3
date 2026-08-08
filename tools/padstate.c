/* padstate.c - poll a gamepad's button/axis state via EVIOCGKEY/EVIOCGABS,
 * the same synchronous sampling capturePad() uses. Prints a line whenever the
 * state changes, for ~15s. Confirms the sampling method independent of re3.
 *
 * Scratch tool; not built by CMake. Usage: padstate /dev/input/event2 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/input.h>

#define BPL (8 * sizeof(long))
static int
tb(const unsigned long *a, int b)
{
	return (a[b / BPL] >> (b % BPL)) & 1UL;
}

int
main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/input/event2";
	int fd = open(dev, O_RDONLY);
	if(fd < 0) {
		perror("open");
		return 1;
	}

	char prev[256] = "";
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for(;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if((now.tv_sec - start.tv_sec) > 15) break;

		unsigned long keys[(KEY_MAX + BPL) / BPL];
		memset(keys, 0, sizeof(keys));
		ioctl(fd, EVIOCGKEY(sizeof(keys)), keys);

		struct input_absinfo ai;
		int lx = 128, ly = 128, l2 = 0, r2 = 0, hx = 0, hy = 0;
		if(ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) lx = ai.value;
		if(ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) ly = ai.value;
		if(ioctl(fd, EVIOCGABS(ABS_Z), &ai) == 0) l2 = ai.value;
		if(ioctl(fd, EVIOCGABS(ABS_RZ), &ai) == 0) r2 = ai.value;
		if(ioctl(fd, EVIOCGABS(ABS_HAT0X), &ai) == 0) hx = ai.value;
		if(ioctl(fd, EVIOCGABS(ABS_HAT0Y), &ai) == 0) hy = ai.value;

		char line[256];
		snprintf(line, sizeof(line), "X=%d Y=%d L2=%d R2=%d hat=%d,%d | %s%s%s%s%s%s%s%s", lx, ly, l2, r2, hx, hy, tb(keys, BTN_SOUTH) ? "Cross " : "",
		         tb(keys, BTN_EAST) ? "Circle " : "", tb(keys, BTN_NORTH) ? "Triangle " : "", tb(keys, BTN_WEST) ? "Square " : "",
		         tb(keys, BTN_TL) ? "L1 " : "", tb(keys, BTN_TR) ? "R1 " : "", tb(keys, BTN_SELECT) ? "Create " : "",
		         tb(keys, BTN_START) ? "Options " : "");
		if(strcmp(line, prev) != 0) {
			printf("%s\n", line);
			fflush(stdout);
			strcpy(prev, line);
		}
		usleep(50000);
	}
	close(fd);
	return 0;
}
