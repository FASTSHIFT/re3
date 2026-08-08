/*
 * hud_stats.cpp - system telemetry backend for the GBM HUD (see hud_stats.h).
 *
 * Holds the /proc and /sys files open in a context and re-reads them with
 * lseek(0)+read, avoiding per-frame open/close. CPU load is sampled every call
 * (needs a frame-to-frame delta); clocks/temp/memory are refreshed at ~1Hz.
 */
#if defined RW_GL3 && defined LIBRW_GBM

#include "hud_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

typedef unsigned long long ull;

struct HudStatsCtx {
	// Persistent fds; -1 if the file is absent/unreadable on this system.
	int fdStat;    // /proc/stat            (CPU load, per frame)
	int fdStatm;   // /proc/self/statm      (process RSS)
	int fdMeminfo; // /proc/meminfo         (system memory)
	int fdTemp;    // thermal_zone0/temp    (CPU temp)
	int fdArm;     // cpufreq scaling_cur_freq (ARM clock)
	int fdV3d;     // debugfs v3d clk_rate   (GPU clock)

	// CPU delta state.
	ull prevIdle, prevTotal;

	// Throttle + cached slow metrics.
	int tick;
	int armMhz, v3dMhz, tempMilliC, rssMb, sysMemPct;
};

// Read the whole (small) file into buf via the held fd; returns bytes read or
// -1. Rewinds first so the fd can be reused every call.
static int
slurp(int fd, char *buf, int cap)
{
	if(fd < 0) return -1;
	if(lseek(fd, 0, SEEK_SET) < 0) return -1;
	int n = (int)read(fd, buf, cap - 1);
	if(n < 0) return -1;
	buf[n] = '\0';
	return n;
}

static int
read_cpu_percent(HudStatsCtx *c)
{
	char buf[256];
	if(slurp(c->fdStat, buf, sizeof(buf)) < 0) return -1;
	ull user, nice, sys, idle, iowait, irq, softirq, steal;
	if(sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) != 8) return -1;
	ull curIdle = idle + iowait;
	ull curTotal = user + nice + sys + idle + iowait + irq + softirq + steal;
	ull dIdle = curIdle - c->prevIdle;
	ull dTotal = curTotal - c->prevTotal;
	c->prevIdle = curIdle;
	c->prevTotal = curTotal;
	if(dTotal == 0) return 0;
	return (int)(100 * (dTotal - dIdle) / dTotal);
}

static int
read_arm_mhz(HudStatsCtx *c)
{
	char buf[64];
	if(slurp(c->fdArm, buf, sizeof(buf)) < 0) return 0;
	int khz = atoi(buf);
	int mhz = khz / 1000;
	if(mhz < 100 || mhz > 3000) return 0; // sanity: Pi ARM 600-2000 MHz
	return mhz;
}

static int
read_v3d_mhz(HudStatsCtx *c)
{
	char buf[64];
	if(slurp(c->fdV3d, buf, sizeof(buf)) < 0) return 0;
	long hz = atol(buf);
	if(hz < 1000000L || hz > 2000000000L) return 0; // 1MHz..2GHz
	return (int)(hz / 1000000);
}

static int
read_temp_milli(HudStatsCtx *c)
{
	char buf[32];
	if(slurp(c->fdTemp, buf, sizeof(buf)) < 0) return -1;
	return atoi(buf);
}

static int
read_rss_mb(HudStatsCtx *c)
{
	char buf[128];
	if(slurp(c->fdStatm, buf, sizeof(buf)) < 0) return -1;
	long totalPages = 0, rssPages = 0;
	if(sscanf(buf, "%ld %ld", &totalPages, &rssPages) != 2) return -1;
	long pg = sysconf(_SC_PAGESIZE);
	return (int)((rssPages * pg) / (1024 * 1024));
}

static int
read_sys_mem_pct(HudStatsCtx *c)
{
	char buf[2048];
	if(slurp(c->fdMeminfo, buf, sizeof(buf)) < 0) return -1;
	long memTotal = 0, memAvail = 0;
	const char *p = buf;
	while(*p) {
		if(strncmp(p, "MemTotal:", 9) == 0)
			memTotal = atol(p + 9);
		else if(strncmp(p, "MemAvailable:", 13) == 0) {
			memAvail = atol(p + 13);
			break;
		}
		const char *nl = strchr(p, '\n');
		if(!nl) break;
		p = nl + 1;
	}
	if(memTotal <= 0) return -1;
	return (int)(100 * (memTotal - memAvail) / memTotal);
}

HudStatsCtx *
HudStats_Create(void)
{
	HudStatsCtx *c = (HudStatsCtx *)calloc(1, sizeof(HudStatsCtx));
	if(!c) return 0;
	c->fdStat = open("/proc/stat", O_RDONLY | O_CLOEXEC);
	c->fdStatm = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
	c->fdMeminfo = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
	c->fdTemp = open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY | O_CLOEXEC);
	c->fdArm = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY | O_CLOEXEC);
	// V3D clock: try the two known debugfs paths.
	c->fdV3d = open("/sys/kernel/debug/clk/fw-clk-v3d/clk_rate", O_RDONLY | O_CLOEXEC);
	if(c->fdV3d < 0) c->fdV3d = open("/sys/kernel/debug/clk/v3d/clk_rate", O_RDONLY | O_CLOEXEC);
	c->armMhz = c->v3dMhz = 0;
	c->tempMilliC = c->rssMb = c->sysMemPct = -1;
	c->tick = 0;
	return c;
}

void
HudStats_Destroy(HudStatsCtx *c)
{
	if(!c) return;
	int fds[] = {c->fdStat, c->fdStatm, c->fdMeminfo, c->fdTemp, c->fdArm, c->fdV3d};
	for(int i = 0; i < (int)(sizeof(fds) / sizeof(fds[0])); i++)
		if(fds[i] >= 0) close(fds[i]);
	free(c);
}

void
HudStats_Sample(HudStatsCtx *c, HudStatsSample *out)
{
	// CPU load every call (needs the frame-to-frame delta).
	out->cpuPct = read_cpu_percent(c);

	// Clocks/temp/memory only ~1Hz; cache the rest of the time.
	if((c->tick++ % 60) == 0) {
		c->armMhz = read_arm_mhz(c);
		c->v3dMhz = read_v3d_mhz(c);
		c->tempMilliC = read_temp_milli(c);
		c->rssMb = read_rss_mb(c);
		c->sysMemPct = read_sys_mem_pct(c);
	}
	out->armMhz = c->armMhz;
	out->v3dMhz = c->v3dMhz;
	out->tempMilliC = c->tempMilliC;
	out->rssMb = c->rssMb;
	out->sysMemPct = c->sysMemPct;
}

#endif
