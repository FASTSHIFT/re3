/* scancost.c - measure the cost of one /dev/input rescan pass like the evdev
 * hotplug health check does: for every eventN, open + a few EVIOCGBIT ioctls +
 * close. Prints per-node and total time. Confirms whether the ~1s rescan can
 * stall the render loop.
 *
 * Scratch tool; not built by CMake. Usage: scancost [iterations] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/input.h>

#define BPL (8 * sizeof(long))
#define NL(x) (((x) + BPL - 1) / BPL)

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double
probe_one(const char *node)
{
	char path[300];
	snprintf(path, sizeof(path), "/dev/input/%s", node);
	double t0 = now_ms();
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if(fd < 0) return -1;
	unsigned long ev[NL(EV_MAX)], key[NL(KEY_MAX)], abs[NL(ABS_MAX)], rel[NL(REL_MAX)];
	char nm[128];
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
	ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
	close(fd);
	return now_ms() - t0;
}

int
main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 1;
	for(int it = 0; it < iters; it++) {
		double total = 0;
		DIR *dir = opendir("/dev/input");
		if(!dir) {
			perror("opendir");
			return 1;
		}
		struct dirent *de;
		while((de = readdir(dir)) != 0) {
			if(strncmp(de->d_name, "event", 5) != 0) continue;
			double t = probe_one(de->d_name);
			if(it == 0) printf("  %-10s %.3f ms\n", de->d_name, t);
			if(t > 0) total += t;
		}
		closedir(dir);
		printf("iter %d: full scan = %.3f ms\n", it, total);
	}
	return 0;
}
