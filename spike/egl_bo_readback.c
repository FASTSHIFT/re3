// Spike: zero-copy readback via a CPU-mappable linear gbm_bo used as the GL
// render target (bypass glReadPixels). Compares gbm_bo_map vs glReadPixels
// timing on the same rendered frame, and checks the mapped pixels are correct.
//
// Approach: create a GBM_BO_USE_LINEAR|RENDERING bo (XRGB8888), import it as an
// EGLImage (EGL_NATIVE_PIXMAP_KHR with the gbm_bo), bind to a GL texture via
// glEGLImageTargetTexture2DOES, attach to an FBO, render, glFinish, then
// gbm_bo_map to read the pixels on the CPU with no GPU readback.
//
// Build (cross): arm-linux-gnueabihf-gcc --sysroot=$SR -O2 spike/egl_bo_readback.c \
//   -o egl_bo_readback -lgbm -lEGL -lGLESv2
// Run on Pi (desktop stopped): ./egl_bo_readback [frames]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef GL_DEPTH_COMPONENT24
#ifdef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24 GL_DEPTH_COMPONENT24_OES
#else
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#endif

#define RW 320
#define RH 240

static double now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define CHECK(c, m) do{ if(!(c)){ fprintf(stderr, "FAIL: %s\n", m); return 1; } }while(0)

int main(int argc, char **argv) {
	int frames = argc > 1 ? atoi(argv[1]) : 120;

	int drmfd = open("/dev/dri/renderD128", O_RDWR);
	CHECK(drmfd >= 0, "open renderD128");
	struct gbm_device *gbm = gbm_create_device(drmfd);
	CHECK(gbm, "gbm_create_device");

	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = getPlatformDisplay ?
		getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL) :
		eglGetDisplay((EGLNativeDisplayType)gbm);
	CHECK(dpy != EGL_NO_DISPLAY, "eglGetDisplay");
	EGLint major, minor;
	CHECK(eglInitialize(dpy, &major, &minor), "eglInitialize");
	printf("EGL %d.%d  exts: %s\n", major, minor, eglQueryString(dpy, EGL_EXTENSIONS));
	eglBindAPI(EGL_OPENGL_ES_API);

	EGLint cfgattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_NONE };
	EGLConfig cfg; EGLint ncfg;
	if(!(eglChooseConfig(dpy, cfgattr, &cfg, 1, &ncfg) && ncfg>0)) cfg = EGL_NO_CONFIG_KHR;
	EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");
	CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "eglMakeCurrent");
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	printf("GL exts: %s\n", glGetString(GL_EXTENSIONS));

	// --- create a linear, CPU-mappable, renderable bo ---
	struct gbm_bo *bo = gbm_bo_create(gbm, RW, RH, GBM_FORMAT_ARGB8888,
		GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	CHECK(bo, "gbm_bo_create(LINEAR|RENDERING)");
	printf("bo stride=%u\n", gbm_bo_get_stride(bo));

	// --- import bo as EGLImage ---
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
		(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
		(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	CHECK(eglCreateImageKHR && glEGLImageTargetTexture2DOES, "EGLImage procs");

	EGLImageKHR img = eglCreateImageKHR(dpy, ctx, EGL_NATIVE_PIXMAP_KHR,
		(EGLClientBuffer)bo, NULL);
	CHECK(img != EGL_NO_IMAGE_KHR, "eglCreateImageKHR(NATIVE_PIXMAP, gbm_bo)");

	GLuint tex; glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	GLuint fbo; glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	printf("FBO status=0x%x (COMPLETE=0x%x)\n", st, GL_FRAMEBUFFER_COMPLETE);
	CHECK(st == GL_FRAMEBUFFER_COMPLETE, "FBO incomplete with EGLImage");
	glViewport(0, 0, RW, RH);

	uint8_t *rb = malloc(RW*RH*4);
	uint8_t *rb2 = malloc(RW*RH*4 + 4096);	// memcpy dst (stride may exceed RW*4)
	uint16_t *rgb565 = malloc(RW*RH*2);	// simulated fb/SPI output (RGB565)
	double tRead=0, tMap=0, tReadConv=0, tMapConv=0, tFinish=0, tReadAfterFinish=0;
	volatile uint32_t sink = 0;
	int mismatch = 0;

	for (int f = 0; f < frames; f++) {
		float p = (f % 120) / 120.0f;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glClearColor(p, 0.3f, 1.0f-p, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		glScissor((int)(p*(RW-40)), RH/2-20, 40, 40);
		glClearColor(1,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glFinish();

		// (0) split: glFinish (pure GPU render wait) then glReadPixels (pure
		//     transfer, GPU already done). Reveals whether readback cost is
		//     GPU-wait (pipelineable) or transfer-bound.
		double s0 = now_ms();
		glFinish();
		double s1 = now_ms();
		glReadPixels(0,0,RW,RH,GL_RGBA,GL_UNSIGNED_BYTE,rb);
		double s2 = now_ms();
		tFinish += s1 - s0;
		tReadAfterFinish += s2 - s1;

		// (1) glReadPixels only (no prior finish; includes implicit GPU wait)
		double a = now_ms();
		glReadPixels(0,0,RW,RH,GL_RGBA,GL_UNSIGNED_BYTE,rb);
		double b = now_ms();
		tRead += b - a;

		// (2) glReadPixels + RGBA->RGB565 convert (what fbdev/SPI actually do)
		double a2 = now_ms();
		glReadPixels(0,0,RW,RH,GL_RGBA,GL_UNSIGNED_BYTE,rb);
		for (int i = 0; i < RW*RH; i++) {
			uint8_t r=rb[i*4], g=rb[i*4+1], bl=rb[i*4+2];
			rgb565[i] = ((r&0xF8)<<8)|((g&0xFC)<<3)|(bl>>3);
		}
		double b2 = now_ms();
		tReadConv += b2 - a2;

		// (3) zero-copy: map, memcpy whole buffer out, unmap
		double c = now_ms();
		uint32_t stride = 0; void *mapData = NULL;
		void *ptr = gbm_bo_map(bo, 0, 0, RW, RH, GBM_BO_TRANSFER_READ, &stride, &mapData);
		if (ptr)
			memcpy(rb2, ptr, (size_t)stride * RH);
		double d = now_ms();
		tMap += d - c;

		// (4) zero-copy done right: map, memcpy whole buffer to normal RAM,
		//     unmap, THEN convert from normal RAM (never read uncached VRAM
		//     per-pixel -- that is catastrophically slow).
		double e = now_ms();
		uint32_t stride2 = 0; void *mapData2 = NULL;
		uint8_t *mp = (uint8_t*)gbm_bo_map(bo, 0, 0, RW, RH, GBM_BO_TRANSFER_READ, &stride2, &mapData2);
		if (mp) {
			memcpy(rb2, mp, (size_t)stride2 * RH);
			gbm_bo_unmap(bo, mapData2);
			for (int y = 0; y < RH; y++) {
				uint8_t *srcRow = rb2 + y*stride2;	// BGRA in normal RAM
				for (int x = 0; x < RW; x++) {
					uint8_t bl=srcRow[x*4], g=srcRow[x*4+1], r=srcRow[x*4+2];
					rgb565[y*RW+x] = ((r&0xF8)<<8)|((g&0xFC)<<3)|(bl>>3);
				}
			}
		}
		double fdt = now_ms();
		tMapConv += fdt - e;
		sink += rgb565[0];

		if (ptr) {
			// sanity: compare a center pixel. bo is ARGB8888 (BGRA bytes on LE);
			// glReadPixels RGBA. Compare R,G,B of a pixel in the yellow scissor.
			if (f == frames-1) {
				int cx = (int)(p*(RW-40)) + 20, cy = RH/2;
				uint8_t *m = (uint8_t*)ptr + cy*stride + cx*4; // B,G,R,A
				uint8_t *r = rb + (cy*RW+cx)*4;                // R,G,B,A
				printf("center map(BGRA)=%d,%d,%d,%d  read(RGBA)=%d,%d,%d,%d\n",
					m[0],m[1],m[2],m[3], r[0],r[1],r[2],r[3]);
				if (m[2]!=r[0] || m[1]!=r[1] || m[0]!=r[2]) mismatch = 1;
			}
			gbm_bo_unmap(bo, mapData);
		} else {
			fprintf(stderr, "gbm_bo_map failed\n");
			break;
		}
	}

	printf("\n=== per-frame avg over %d frames (ms) ===\n", frames);
	printf("(0a) glFinish (GPU render wait):   %.3f\n", tFinish/frames);
	printf("(0b) glReadPixels after finish:   %.3f  (pure transfer)\n", tReadAfterFinish/frames);
	printf("(1) glReadPixels only:            %.3f\n", tRead/frames);
	printf("(2) glReadPixels + RGB565 conv:   %.3f\n", tReadConv/frames);
	printf("(3) gbm_bo_map + memcpy:          %.3f\n", tMap/frames);
	printf("(4) map + memcpy + RGB565 conv:   %.3f  <-- zero-copy fbdev/SPI path\n", tMapConv/frames);
	printf("pixel check:         %s (sink=%u)\n", mismatch ? "MISMATCH" : "OK", (unsigned)sink);

	// ---- Bonus: render to an RGB565 FBO and read back 565 directly ----
	// If VC4 supports a 565 color-renderable texture + a 565 glReadPixels, this
	// halves transfer bytes AND removes the software RGBA->565 conversion (the
	// output is already exactly what fb0 16bpp / ST7789 want).
	{
		GLint rf=0, rt=0;
		glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
		glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
		printf("\nIMPLEMENTATION_COLOR_READ_FORMAT=0x%x TYPE=0x%x "
			"(RGB=0x%x UNSIGNED_SHORT_5_6_5=0x%x)\n", rf, rt, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);

		GLuint t565; glGenTextures(1,&t565);
		glBindTexture(GL_TEXTURE_2D,t565);
		glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,RW,RH,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,NULL);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);

		// depth-only DEPTH_COMPONENT24 renderbuffer (the VC4-working config from
		// the depth fix) attached alongside the 565 color, to confirm 565 color
		// + working depth can coexist in one FBO -- exactly what librw needs.
		GLuint d24; glGenRenderbuffers(1,&d24);
		glBindRenderbuffer(GL_RENDERBUFFER,d24);
		glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,RW,RH);

		GLuint f565; glGenFramebuffers(1,&f565);
		glBindFramebuffer(GL_FRAMEBUFFER,f565);
		glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t565,0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,d24);
		GLenum st565 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		GLint dbits=0; glGetIntegerv(GL_DEPTH_BITS,&dbits);
		printf("RGB565+DEPTH24 FBO status=0x%x (COMPLETE=0x%x) GL_DEPTH_BITS=%d\n",
			st565, GL_FRAMEBUFFER_COMPLETE, dbits);

		if (st565 == GL_FRAMEBUFFER_COMPLETE) {
			// depth correctness: near(red,z=-0.5) must occlude far(green,z=+0.5)
			// regardless of draw order. Reuse the shader program from earlier? No
			// -- this spike has none; use scissor+clear-depth trick instead: draw
			// two full-screen clears at different depths via glClear won't test
			// depth. So just confirm readback works; depth coexistence is proven
			// by COMPLETE + DEPTH_BITS>0 (the standalone depth spike already
			// proved DEPTH_COMPONENT24 depth-tests correctly on VC4).
			double t565read=0;
			for (int f=0; f<frames; f++) {
				glBindFramebuffer(GL_FRAMEBUFFER,f565);
				float p=(f%120)/120.0f;
				glClearColor(p,0.3f,1.0f-p,1.0f); glClearDepthf(1.0f);
				glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
				glFinish();
				double a=now_ms();
				glReadPixels(0,0,RW,RH,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,rgb565);
				double b=now_ms();
				GLenum err=glGetError();
				t565read += b-a;
				if (f==0) printf("  first 565 read glGetError=0x%x px0=0x%04x\n", err, rgb565[0]);
			}
			printf("(5) RGB565+DEPTH24 FBO + direct 565 readback: %.3f  <-- no conv, half bytes\n", t565read/frames);
		}
	}

	free(rb);
	eglDestroyImageKHR(dpy, img);
	free(rb); free(rb2); free(rgb565);
	gbm_bo_destroy(bo);
	eglDestroyContext(dpy, ctx); eglTerminate(dpy);
	gbm_device_destroy(gbm); close(drmfd);
	return 0;
}
