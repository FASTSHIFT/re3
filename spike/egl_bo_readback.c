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

#define RW 640
#define RH 480

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
	double tRead=0, tMap=0;
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

		// glReadPixels path (for timing comparison)
		double a = now_ms();
		glReadPixels(0,0,RW,RH,GL_RGBA,GL_UNSIGNED_BYTE,rb);
		double b = now_ms();
		tRead += b - a;

		// zero-copy map path
		double c = now_ms();
		uint32_t stride = 0; void *mapData = NULL;
		void *ptr = gbm_bo_map(bo, 0, 0, RW, RH, GBM_BO_TRANSFER_READ, &stride, &mapData);
		double d = now_ms();
		tMap += d - c;

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
	printf("glReadPixels: %.3f\n", tRead/frames);
	printf("gbm_bo_map:   %.3f\n", tMap/frames);
	printf("pixel check:  %s\n", mismatch ? "MISMATCH" : "OK (map matches readback)");

	free(rb);
	eglDestroyImageKHR(dpy, img);
	gbm_bo_destroy(bo);
	eglDestroyContext(dpy, ctx); eglTerminate(dpy);
	gbm_device_destroy(gbm); close(drmfd);
	return 0;
}
