/*
 * vc4_gpustat.c - V3D GPU hardware performance counter tool for RPi Zero 2 W
 *
 * Directly programs the V3D PCTR hardware registers via /dev/mem mmap
 * (no DRM perfmon API needed - works around the VC4 driver not exposing
 * V3D_PERFMON_SET_GLOBAL).
 *
 * Samples at fixed intervals and prints QPU utilisation, texture cache
 * efficiency, and a CPU-vs-GPU bottleneck verdict.
 *
 * V3D register layout (BCM2837, peri base 0x3f000000):
 *   V3D base: 0x3fc00000 (bus 0x7ec00000, peri offset 0xc00000)
 *   V3D_IDENT0  @ 0x000  -- identifies GPU
 *   V3D_BFC     @ 0x044  -- Binning Frame Count
 *   V3D_RFC     @ 0x048  -- Rendering Frame Count
 *   V3D_PCTRC   @ 0x670  -- clear counters (write 1 to bit)
 *   V3D_PCTRE   @ 0x674  -- enable counters + select source (EN=bit31)
 *   V3D_PCTR(n) @ 0x680 + n*8   -- counter value
 *   V3D_PCTRS(n)@ 0x684 + n*8   -- counter source select (0-29)
 *
 * QPU counter sources (V3D_PCTRS values):
 *   0  FEP valid prims no render
 *   1  FEP valid prims render
 *   6  TLB quads not passing Z
 *   7  TLB quads passing Z+stencil
 *   9  TLB quads written to color buf
 *  13  QPU total idle cycles
 *  14  QPU total cycles vertex/coord shading
 *  15  QPU total cycles fragment shading
 *  16  QPU total valid instruction cycles
 *  17  QPU total cycles stalled on TMU
 *  18  QPU total cycles stalled on scoreboard
 *  20  TMU total texture quads processed
 *  21  TMU total texture cache misses
 *  28  L2C total L2 cache hits
 *  29  L2C total L2 cache misses
 *
 * Usage: sudo ./vc4_gpustat [interval_ms] [samples]
 *
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- V3D register layout ---- */
#define V3D_PHYS_BASE   0x3fc00000u   /* BCM2837 (Pi 2/3/Zero2W) */
#define V3D_MAP_SIZE    0x1000u

#define OFF_IDENT0  0x000
#define OFF_BFC     0x044
#define OFF_RFC     0x048
#define OFF_PCTRC   0x670
#define OFF_PCTRE   0x674
#define OFF_PCTR(n) (0x680 + (n)*8)
#define OFF_PCTRS(n)(0x684 + (n)*8)

/* Counter sources we want */
#define SRC_FEP_PRIMS_RENDER   1
#define SRC_TLB_QUADS_PASS_Z   7
#define SRC_TLB_QUADS_WRITTEN  9
#define SRC_QPU_IDLE          13
#define SRC_QPU_VERT          14
#define SRC_QPU_FRAG          15
#define SRC_QPU_VALID_INST    16
#define SRC_QPU_TMU_STALL     17
#define SRC_QPU_SB_STALL      18
#define SRC_TMU_PROCESSED     20
#define SRC_TMU_MISS          21
#define SRC_L2_HIT            28
#define SRC_L2_MISS           29

/* Up to 16 hardware counters on VC4 */
#define NCTR 13

static const struct { uint8_t src; const char *name; } ctrs[NCTR] = {
    { SRC_QPU_IDLE,         "qpu_idle"       },
    { SRC_QPU_FRAG,         "qpu_frag"       },
    { SRC_QPU_VERT,         "qpu_vert"       },
    { SRC_QPU_VALID_INST,   "qpu_valid_inst" },
    { SRC_QPU_TMU_STALL,    "qpu_tmu_stall"  },
    { SRC_QPU_SB_STALL,     "qpu_sb_stall"   },
    { SRC_TMU_PROCESSED,    "tmu_quads"      },
    { SRC_TMU_MISS,         "tmu_miss"       },
    { SRC_L2_HIT,           "l2_hit"         },
    { SRC_L2_MISS,          "l2_miss"        },
    { SRC_TLB_QUADS_WRITTEN,"tlb_written"    },
    { SRC_TLB_QUADS_PASS_Z, "tlb_pass_z"     },
    { SRC_FEP_PRIMS_RENDER, "fep_prims"      },
};

static volatile uint32_t *v3d = NULL;

static uint32_t rd(unsigned off) { return v3d[off/4]; }
static void     wr(unsigned off, uint32_t v) { v3d[off/4] = v; }

static int init_v3d(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem (need root)"); return -1; }
    void *m = mmap(NULL, V3D_MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED,
                   fd, V3D_PHYS_BASE);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap V3D"); return -1; }
    v3d = (volatile uint32_t *)m;

    uint32_t id = rd(OFF_IDENT0);
    if ((id & 0xFFFFFF) != 0x443356) {  /* "V3D" in ASCII */
        fprintf(stderr, "IDENT0=0x%08x - doesn't look like V3D at 0x%08x\n",
                id, V3D_PHYS_BASE);
        return -1;
    }
    printf("V3D IDENT0=0x%08x  rev=%d  slices=%d  qpus/slice=%d\n",
           id, (id>>24)&0xf, (rd(0x004)>>4)&0xf, rd(0x004)&0xf);
    return 0;
}

static void setup_counters(void)
{
    /* Disable all counters */
    wr(OFF_PCTRE, 0);
    /* Clear counters */
    wr(OFF_PCTRC, (1u << NCTR) - 1);
    /* Configure sources */
    for (int i = 0; i < NCTR; i++)
        wr(OFF_PCTRS(i), ctrs[i].src);
    /* Enable counters (bit 31 = EN, bits 0..15 = enable mask) */
    wr(OFF_PCTRE, 0x80000000u | ((1u << NCTR) - 1));
}

static void read_counters(uint32_t *out)
{
    for (int i = 0; i < NCTR; i++)
        out[i] = rd(OFF_PCTR(i));
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_report(const uint32_t *v0, const uint32_t *v1,
                         uint32_t bfc0, uint32_t bfc1,
                         uint32_t rfc0, uint32_t rfc1,
                         double dt, int sample)
{
    /* Counters wrap at 2^32, delta handles wrap naturally for uint32 */
    uint32_t d[NCTR];
    for (int i = 0; i < NCTR; i++)
        d[i] = v1[i] - v0[i];

/* Named access by position */
#define QPU_IDLE     d[0]
#define QPU_FRAG     d[1]
#define QPU_VERT     d[2]
#define QPU_VALID    d[3]
#define QPU_TMU_ST   d[4]
#define QPU_SB_ST    d[5]
#define TMU_TOTAL    d[6]
#define TMU_MISS     d[7]
#define L2_HIT       d[8]
#define L2_MISS      d[9]
#define TLB_WRITTEN  d[10]
#define TLB_PASS_Z   d[11]
#define FEP_PRIMS    d[12]

    uint32_t frames = rfc1 - rfc0;
    uint64_t qpu_active = (uint64_t)QPU_FRAG + QPU_VERT;
    uint64_t qpu_total  = (uint64_t)QPU_IDLE + qpu_active +
                          QPU_TMU_ST + QPU_SB_ST;

    if (qpu_total < 1000) {
        printf("[sample %d] Negligible QPU activity. Is re3 in gameplay?\n\n", sample);
        return;
    }

    double pct_idle  = 100.0 * QPU_IDLE   / qpu_total;
    double pct_frag  = 100.0 * QPU_FRAG   / qpu_total;
    double pct_vert  = 100.0 * QPU_VERT   / qpu_total;
    double pct_tmu   = 100.0 * QPU_TMU_ST / qpu_total;
    double pct_sb    = 100.0 * QPU_SB_ST  / qpu_total;

    double tmu_miss_pct = TMU_TOTAL ? 100.0 * TMU_MISS / TMU_TOTAL : 0.0;
    uint64_t l2_total   = (uint64_t)L2_HIT + L2_MISS;
    double l2_miss_pct  = l2_total ? 100.0 * L2_MISS / l2_total : 0.0;

    uint64_t tlb_total  = (uint64_t)TLB_WRITTEN + TLB_PASS_Z;
    double depth_pass   = tlb_total ? 100.0 * TLB_WRITTEN / tlb_total : 0.0;

    printf("--- sample %d  %.2fs window  %u render frames (%.0f fps) ---\n",
           sample, dt, frames, frames / dt);
    printf("\n");

    printf("  QPU total cycles : %"PRIu64"\n", qpu_total);
    printf("  QPU idle         : %5.1f%%  (GPU utilisation = %.1f%%)\n",
           pct_idle, 100.0 - pct_idle);
    printf("  QPU frag shader  : %5.1f%%\n", pct_frag);
    printf("  QPU vert shader  : %5.1f%%\n", pct_vert);
    printf("  QPU TMU stall    : %5.1f%%  <- texture fetch latency\n", pct_tmu);
    printf("  QPU SB stall     : %5.1f%%  <- WAW/WAR dependency\n", pct_sb);
    printf("\n");
    printf("  TMU texture quads: %u  (%.0fk/s)\n",
           TMU_TOTAL, TMU_TOTAL / dt / 1000.0);
    printf("  TMU cache miss   : %5.1f%%\n", tmu_miss_pct);
    printf("  L2 cache miss    : %5.1f%%\n", l2_miss_pct);
    printf("\n");
    printf("  TLB quads written: %u  (%.0f Mpx/s)\n",
           TLB_WRITTEN, TLB_WRITTEN * 4.0 / dt / 1e6);
    printf("  Depth pass rate  : %5.1f%%  (%.1f%% culled)\n",
           depth_pass, 100.0 - depth_pass);
    printf("  Primitives/frame : %.0f\n",
           frames ? (double)FEP_PRIMS / frames : 0.0);
    printf("\n");

    /* ---- Bottleneck verdict ---- */
    printf("  VERDICT:\n");
    if (pct_idle > 70.0) {
        printf("    GPU is mostly IDLE (%.0f%%) -- CPU is the bottleneck.\n",
               pct_idle);
        printf("    Potential gains: reduce CPU game-logic cost, not shader work.\n");
    } else if (pct_idle > 40.0) {
        printf("    GPU moderately loaded (%.0f%% idle) -- mixed CPU/GPU.\n", pct_idle);
    } else {
        printf("    GPU is BUSY (%.0f%% idle).\n", pct_idle);
        if (pct_tmu > 20.0)
            printf("    Primary stall: TMU (%.0f%%) -- texture bandwidth bound.\n", pct_tmu);
        else if (pct_frag > pct_vert * 2)
            printf("    Primary load: fragment shading (fill-rate bound).\n");
        else
            printf("    Primary load: vertex shading (geometry bound).\n");
    }
    printf("\n");

#undef QPU_IDLE
#undef QPU_FRAG
#undef QPU_VERT
#undef QPU_VALID
#undef QPU_TMU_ST
#undef QPU_SB_ST
#undef TMU_TOTAL
#undef TMU_MISS
#undef L2_HIT
#undef L2_MISS
#undef TLB_WRITTEN
#undef TLB_PASS_Z
#undef FEP_PRIMS
}

int main(int argc, char **argv)
{
    int interval_ms = (argc > 1) ? atoi(argv[1]) : 2000;
    int samples     = (argc > 2) ? atoi(argv[2]) : 5;

    if (init_v3d() < 0) return 1;

    setup_counters();

    printf("\n=== V3D GPU performance monitor ===\n");
    printf("Interval: %dms  Samples: %d  Counters: %d\n\n",
           interval_ms, samples, NCTR);

    uint32_t v0[NCTR], v1[NCTR];
    uint32_t bfc0, bfc1, rfc0, rfc1;

    for (int i = 0; i < samples; i++) {
        read_counters(v0);
        bfc0 = rd(OFF_BFC);
        rfc0 = rd(OFF_RFC);
        double t0 = now_sec();

        struct timespec ts = {
            interval_ms / 1000,
            (long)(interval_ms % 1000) * 1000000L
        };
        nanosleep(&ts, NULL);

        read_counters(v1);
        bfc1 = rd(OFF_BFC);
        rfc1 = rd(OFF_RFC);
        double dt = now_sec() - t0;

        print_report(v0, v1, bfc0, bfc1, rfc0, rfc1, dt, i + 1);
    }

    /* Disable counters cleanly */
    wr(OFF_PCTRE, 0);
    return 0;
}
