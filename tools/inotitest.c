/* inotitest.c - watch /dev/input for CREATE/DELETE exactly like the evdev
 * input source does, to confirm inotify actually delivers node add/remove
 * events on this system. Prints events for ~30s.
 *
 * Scratch tool; not built by CMake. Usage: inotitest [seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/inotify.h>
#include <time.h>

int
main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	int fd = inotify_init1(IN_NONBLOCK);
	if(fd < 0) {
		perror("inotify_init1");
		return 1;
	}
	int wd = inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_DELETE);
	if(wd < 0) {
		perror("inotify_add_watch");
		return 1;
	}
	printf("watching /dev/input for %ds...\n", secs);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for(;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if((now.tv_sec - start.tv_sec) > secs) break;
		ssize_t n = read(fd, buf, sizeof(buf));
		if(n <= 0) {
			usleep(50000);
			continue;
		}
		for(char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if(ev->len > 0) printf("  %s %s\n", (ev->mask & IN_CREATE) ? "CREATE" : (ev->mask & IN_DELETE) ? "DELETE" : "?", ev->name);
			fflush(stdout);
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
	printf("done.\n");
	return 0;
}
