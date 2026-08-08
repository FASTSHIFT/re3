/*
 * sink_spi.cpp - OutputSink pushing frames to an ST7789 SPI panel.
 *
 * Uses drivers/st7789. Scales the readback frame to the panel, flips to
 * top-down. Color/clock knobs are env-tunable:
 *   RE3_SPI_INVERT, RE3_SPI_ENDIAN, RE3_SPI_ROT, RE3_SPI_HZ, RE3_SPI_BGR.
 *
 * Double-buffered SPI push: the main thread fills one pixel buffer while the
 * background SPI thread pushes the previous frame. On the next spi_present()
 * call the main thread waits for the SPI thread (if not done yet), then swaps.
 * This hides the ~19ms SPI flush behind GPU rendering, raising effective fps
 * from 1/(GPU+SPI) to 1/max(GPU, SPI). See docs/06.
 *
 * Active when RE3_OUTPUT_SPI is defined.
 */
#if defined RW_GL3 && defined LIBRW_GBM && defined(RE3_OUTPUT_SPI)

#include "output_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

#include "st7789.h"

// ---- state ----------------------------------------------------------------
static st7789_t *sDev = 0;
static int sW = 0, sH = 0;
static int sSwapRB = 0;

// Double-buffering: two panel-sized RGB565 buffers ping-pong between the
// fill path (main thread) and the flush path (SPI thread).
static uint16_t *sBuf[2] = {0, 0}; // [0] = being filled, [1] = being flushed
static int sFillIdx = 0;           // which buf the main thread writes into

// SPI background thread
static pthread_t sSpiThread;
static sem_t sSpiWork; // main thread -> SPI: "new frame ready"
static sem_t sSpiDone; // SPI -> main thread: "flush complete"
static int sSpiStop = 0;
static int sSpiInited = 0;

// ---- SPI background thread ------------------------------------------------
static void *
spi_thread_func(void *)
{
	while(1) {
		sem_wait(&sSpiWork);
		if(sSpiStop) break;
		// Flush the buffer the main thread just handed us (the OTHER index).
		st7789_flush(sDev, sBuf[1 - sFillIdx]);
		sem_post(&sSpiDone);
	}
	sem_post(&sSpiDone); // unblock terminate()
	return 0;
}

// ---- dump helper ----------------------------------------------------------
static void
spi_dump(const uint16_t *buf, int w, int h)
{
	static const char *dir = 0;
	static int init = 0, n = 0, every = 30;
	if(!init) {
		init = 1;
		dir = getenv("RE3_SPI_DUMP");
		const char *e = getenv("RE3_SPI_DUMP_EVERY");
		if(e) every = atoi(e);
		if(every < 1) every = 1;
	}
	if(!dir || (n++ % every) != 0) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/frame%04d.ppm", dir, n / every);
	FILE *f = fopen(path, "wb");
	if(!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for(int i = 0; i < w * h; i++) {
		uint16_t p = buf[i];
		uint8_t rgb[3];
		rgb[0] = (uint8_t)((p >> 11 & 0x1F) << 3);
		rgb[1] = (uint8_t)((p >> 5 & 0x3F) << 2);
		rgb[2] = (uint8_t)((p & 0x1F) << 3);
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

// ---- sink API -------------------------------------------------------------
static bool
spi_init(int renderW, int renderH)
{
	(void)renderW;
	(void)renderH;

	st7789_config_t cfg;
	st7789_config_default(&cfg);
	cfg.little_endian = 1; // GLES GL_UNSIGNED_SHORT_5_6_5 on LE host = correct

	const char *e;
	if((e = getenv("RE3_SPI_INVERT")) != 0) cfg.invert = atoi(e);
	if((e = getenv("RE3_SPI_ENDIAN")) != 0) cfg.little_endian = atoi(e);
	if((e = getenv("RE3_SPI_ROT")) != 0) cfg.rotation = (st7789_rotation_t)atoi(e);
	if((e = getenv("RE3_SPI_HZ")) != 0) cfg.spi_hz = (uint32_t)strtoul(e, 0, 10);
	if((e = getenv("RE3_SPI_BGR")) != 0) sSwapRB = atoi(e);

	st7789_tune_system(65536);

	sDev = st7789_open(&cfg);
	if(sDev == 0) {
		printf("spi: st7789_open failed\n");
		return false;
	}
	sW = st7789_get_width(sDev);
	sH = st7789_get_height(sDev);

	sBuf[0] = (uint16_t *)malloc((size_t)sW * sH * 2);
	sBuf[1] = (uint16_t *)malloc((size_t)sW * sH * 2);
	if(!sBuf[0] || !sBuf[1]) return false;
	sFillIdx = 0;

	// Semaphores: done starts at 1 (SPI thread "ready to accept first frame").
	sem_init(&sSpiWork, 0, 0);
	sem_init(&sSpiDone, 0, 1);
	sSpiStop = 0;
	pthread_create(&sSpiThread, 0, spi_thread_func, 0);
	sSpiInited = 1;

	printf("spi: ST7789 %dx%d (double-buffered)\n", sW, sH);
	return true;
}

static inline void
fill_buf(uint16_t *dst, const uint16_t *rgb565, int w, int h)
{
	if(w == sW && h == sH && !sSwapRB) {
		// Fast path: render size == panel, no channel swap.
		// Rows are bottom-up in readback; memcpy them reversed.
		for(int py = 0; py < sH; py++) memcpy(dst + py * sW, rgb565 + (h - 1 - py) * w, (size_t)sW * 2);
	} else {
		for(int py = 0; py < sH; py++) {
			int ry = (py * h) / sH;
			const uint16_t *srcRow = rgb565 + (h - 1 - ry) * w;
			uint16_t *dstRow = dst + py * sW;
			for(int px = 0; px < sW; px++) {
				uint16_t p = srcRow[(px * w) / sW];
				dstRow[px] = sSwapRB ? (uint16_t)(((p & 0x1F) << 11) | ((p >> 5 & 0x3F) << 5) | ((p >> 11) & 0x1F)) : p;
			}
		}
	}
}

static void
spi_present(const uint16_t *rgb565, int w, int h)
{
	if(sDev == 0 || !sBuf[0] || sW <= 0 || sH <= 0) return;

	// Wait for the SPI thread to finish pushing the previous frame.
	// On the very first call sSpiDone starts at 1 so this returns immediately.
	sem_wait(&sSpiDone);

	// Fill the buffer the SPI thread just vacated (sFillIdx).
	fill_buf(sBuf[sFillIdx], rgb565, w, h);
	spi_dump(sBuf[sFillIdx], sW, sH);

	// Swap: the filled buffer becomes the flush target; the other is now free
	// for the next fill while the SPI thread pushes.
	sFillIdx ^= 1;

	// Signal the SPI thread to flush the just-filled buffer (= 1 - sFillIdx).
	sem_post(&sSpiWork);
	// Note: we do NOT wait here. The main thread returns immediately and the
	// GPU begins rendering frame N+1 while SPI is pushing frame N.
}

static void
spi_terminate(void)
{
	if(sSpiInited) {
		sSpiStop = 1;
		sem_post(&sSpiWork);
		sem_wait(&sSpiDone);
		pthread_join(sSpiThread, 0);
		sem_destroy(&sSpiWork);
		sem_destroy(&sSpiDone);
		sSpiInited = 0;
	}
	if(sDev != 0) {
		st7789_close(sDev);
		sDev = 0;
	}
	free(sBuf[0]);
	sBuf[0] = 0;
	free(sBuf[1]);
	sBuf[1] = 0;
}

static OutputSink sSink = {spi_init, spi_present, spi_terminate, "spi"};

OutputSink *
OutputSink_Get(void)
{
	return &sSink;
}

#endif
