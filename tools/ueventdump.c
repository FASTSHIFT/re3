/* ueventdump.c - dump raw kernel uevent messages from the netlink socket, the
 * same way the evdev input source consumes them. Prints the action line and
 * every KEY=VALUE property, so we can see exactly what an input add@/remove@
 * carries. Watches for ~30s.
 *
 * Scratch tool; not built by CMake. Usage: ueventdump [seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <time.h>

int
main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if(fd < 0) {
		perror("socket");
		return 1;
	}
	struct sockaddr_nl addr;
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = 1;
	if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	printf("watching uevents for %ds...\n", secs);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	char buf[4096];
	for(;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if((now.tv_sec - start.tv_sec) > secs) break;
		ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
		if(n <= 0) {
			usleep(50000);
			continue;
		}
		buf[n] = '\0';

		/* Only show input-related messages to cut noise. */
		int isInput = 0;
		for(size_t off = strlen(buf) + 1; off < (size_t)n; off += strlen(buf + off) + 1)
			if(strcmp(buf + off, "SUBSYSTEM=input") == 0) isInput = 1;
		if(!isInput) continue;

		printf("---- msg (%zd bytes) ----\n", n);
		printf("  HDR: %s\n", buf);
		for(size_t off = strlen(buf) + 1; off < (size_t)n; off += strlen(buf + off) + 1) printf("  %s\n", buf + off);
		fflush(stdout);
	}
	printf("done.\n");
	return 0;
}
