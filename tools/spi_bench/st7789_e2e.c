/*
 * st7789_e2e.c - ST7789 端到端黑白双 buffer 切换刷新帧率测试
 *
 * 复用参考工程驱动 ref_lv_gba_emu/port/rpi/st7789.c（不改一行），
 * 通过本目录的 wiringPi shim 提供 GPIO(/dev/gpiomem) + SPI(spidev) 后端。
 *
 * 接线（BCM GPIO，与 lv_port_rpi.c 一致）：
 *   RST=27  CS=8(CE0)  DC=25  BLK=24
 *   SCLK=11 MOSI=10（SPI0 硬件引脚，由 dtparam=spi=on 分配）
 *
 * 流程：st7789_init -> rotation=1(320x240横屏) -> 交替推整屏黑/白帧，统计 fps。
 *
 * 用法:
 *   [SPI_HZ=100000000] st7789_e2e [frames]
 *   默认 frames=200。SPI_HZ 覆盖驱动硬编码的 60MHz。
 */
#include "rpi/st7789.h"
#include "wiringPi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DISP_RST_PIN 27
#define DISP_CS_PIN 8
#define DISP_DC_PIN 25
#define DISP_BLK_PIN 24

/* rotation=1 时的可视分辨率（横屏）*/
#define SCR_W 320
#define SCR_H 240

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 200;

    if (wiringPiSetupGpio() < 0) {
        fprintf(stderr, "gpio setup failed\n");
        return 1;
    }

    /* 背光打开 */
    pinMode(DISP_BLK_PIN, OUTPUT);
    digitalWrite(DISP_BLK_PIN, 1);

    st7789_t disp;
    printf("st7789 init (hor=%d ver=%d)...\n", SCR_W, SCR_H);
    int ret = st7789_init(&disp, DISP_RST_PIN, DISP_CS_PIN, DISP_DC_PIN, SCR_W, SCR_H);
    if (ret < 0) {
        fprintf(stderr, "st7789_init failed: %d\n", ret);
        return 1;
    }
    st7789_set_rotation(&disp, 1);
    /*
     * 注意：rotation=1 后驱动内部 cur_width/cur_height 被交换成 240x320，
     * 但 MADCTL=0xA0 已把面板设为横屏，gba emu 的 LVGL 画布是 HOR_RES×VER_RES
     * =320×240，实际推屏按 320 宽×240 高寻址（见 lv_port_rpi.c disp_flush_cb）。
     * 因此这里按 320×240 全屏推送，与 gba emu 保持一致（方向正确）。
     */
    printf("driver cur_%dx%d, pushing full frame as %dx%d (landscape, matches gba emu)\n",
        disp.cur_width, disp.cur_height, SCR_W, SCR_H);

    int w = SCR_W;
    int h = SCR_H;
    size_t px = (size_t)w * h;

    /* 两个全屏 buffer：黑 / 白 (RGB565) */
    uint16_t* buf_black = malloc(px * sizeof(uint16_t));
    uint16_t* buf_white = malloc(px * sizeof(uint16_t));
    if (!buf_black || !buf_white) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    for (size_t i = 0; i < px; i++) {
        buf_black[i] = 0x0000; /* 黑 */
        buf_white[i] = 0xFFFF; /* 白 */
    }

    printf("pushing %d frames (alternating black/white)...\n", frames);
    shim_profile_reset();
    double t0 = now_sec();
    for (int i = 0; i < frames; i++) {
        const uint16_t* buf = (i & 1) ? buf_white : buf_black;
        st7789_draw_bitmap(&disp, 0, 0, buf, w, h);
    }
    double dt = now_sec() - t0;
    shim_profile_report(frames);

    double fps = frames / dt;
    double frame_bytes = px * 2.0;
    double mbps = frame_bytes * frames / dt / (1024.0 * 1024.0);

    printf("\n=== result ===\n");
    printf("frames      : %d\n", frames);
    printf("elapsed     : %.3f s\n", dt);
    printf("fps         : %.1f\n", fps);
    printf("per frame   : %.3f ms\n", dt / frames * 1000.0);
    printf("throughput  : %.2f MB/s (%.1f KB/frame)\n", mbps, frame_bytes / 1024.0);

    free(buf_black);
    free(buf_white);
    return 0;
}
