/*
 * hud_stats.h - system telemetry backend for the GBM HUD overlay.
 *
 * Collects CPU load, CPU/GPU clocks, temperature and memory usage from
 * /proc and /sys. Split from the overlay renderer (hud_overlay.*) so the
 * front end only formats/draws and never touches sysfs.
 *
 * The backend keeps the sysfs/proc files OPEN in a context and re-reads them
 * with lseek(0)+read instead of reopening each time, so per-frame CPU load
 * sampling and the throttled clock/temp/memory refresh have no open/close cost.
 */
#ifndef RE3_HUD_STATS_H
#define RE3_HUD_STATS_H

// Opaque backend context: owns persistent file descriptors + CPU delta state.
struct HudStatsCtx;

// Sampled telemetry. A field is <0 (or 0 for clocks) when unavailable.
struct HudStatsSample {
	int cpuPct;     // whole-system CPU load 0-100 (per-frame delta)
	int armMhz;     // ARM core clock MHz (0 = unknown)
	int v3dMhz;     // V3D GPU clock MHz (0 = unknown)
	int tempMilliC; // CPU temperature in milli-degrees C (-1 = unknown)
	int rssMb;      // this process's resident set size, MB (-1 = unknown)
	int sysMemPct;  // system memory used %, 0-100 (-1 = unknown)
};

// Open the backend (persistent fds). Never returns null in practice; missing
// files are handled per-metric. Free with HudStats_Destroy.
HudStatsCtx *
HudStats_Create(void);

void
HudStats_Destroy(HudStatsCtx *ctx);

// Sample telemetry. cpuPct is computed every call (it needs a per-frame delta);
// the heavier clock/temp/memory reads are refreshed on a throttle inside and
// otherwise return the cached values, so this is cheap to call per frame.
void
HudStats_Sample(HudStatsCtx *ctx, HudStatsSample *out);

#endif /* RE3_HUD_STATS_H */
