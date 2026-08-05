// Minimal verification: EGL + GBM (no desktop) -> hardware GLES2 render at
// 320x240 -> glReadPixels -> write to /dev/fb0. Measures per-step cost.
//
// Goal: prove the desktop-free GPU->framebuffer link works on the Pi, and
// measure readback + fb0 blit cost (relevant to later SPI push).
//
// Build (cross): see spike/build.sh
// Run on Pi (desktop stopped): sudo ./egl_fb_test [frames]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define RW 320
#define RH 240

static double now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define CHECK(cond, msg) do{ if(!(cond)){ fprintf(stderr, "FAIL: %s\n", msg); return 1; } }while(0)

int main(int argc, char **argv) {
	int frames = argc > 1 ? atoi(argv[1]) : 120;

	// --- 1. GBM device from the render node ---
	int drmfd = open("/dev/dri/renderD128", O_RDWR);
	CHECK(drmfd >= 0, "open renderD128");
	struct gbm_device *gbm = gbm_create_device(drmfd);
	CHECK(gbm, "gbm_create_device");

	// --- 2. EGL display via GBM platform ---
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy;
	if (getPlatformDisplay)
		dpy = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	else
		dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	CHECK(dpy != EGL_NO_DISPLAY, "eglGetDisplay");

	EGLint major, minor;
	CHECK(eglInitialize(dpy, &major, &minor), "eglInitialize");
	printf("EGL %d.%d\n", major, minor);
	printf("EGL_VENDOR: %s\n", eglQueryString(dpy, EGL_VENDOR));

	eglBindAPI(EGL_OPENGL_ES_API);

	// --- 3. Config + context (surfaceless, we render to an FBO) ---
	// Surfaceless: we render to an FBO, so don't require a window/pbuffer
	// surface type (GBM configs may not advertise PBUFFER_BIT).
	EGLint cfgattr[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg; EGLint ncfg;
	if (!(eglChooseConfig(dpy, cfgattr, &cfg, 1, &ncfg) && ncfg > 0)) {
		// Fallback: configless context (EGL_MESA_configless_context)
		fprintf(stderr, "eglChooseConfig empty, trying configless\n");
		cfg = EGL_NO_CONFIG_KHR;
	}

	EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");

	CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "eglMakeCurrent(surfaceless)");

	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));

	// --- 4. Offscreen FBO at 320x240 ---
	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, RW, RH, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "FBO incomplete");
	glViewport(0, 0, RW, RH);

	// --- 5. Open /dev/fb0 (OPTIONAL: absent when no HDMI; SPI path doesn't need it) ---
	int fbfd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	uint8_t *fbp = NULL;
	size_t fbsize = 0;
	int have_fb = 0;
	if (fbfd >= 0 &&
	    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
		printf("fb0: %dx%d %dbpp line=%d\n", vinfo.xres, vinfo.yres,
			vinfo.bits_per_pixel, finfo.line_length);
		fbsize = finfo.line_length * vinfo.yres;
		fbp = mmap(NULL, fbsize, PROT_READ|PROT_WRITE, MAP_SHARED, fbfd, 0);
		if (fbp != MAP_FAILED) have_fb = 1;
		else { fbp = NULL; printf("WARN: mmap fb0 failed (no display?)\n"); }
	} else {
		printf("WARN: /dev/fb0 unavailable (no HDMI?) - testing render+readback only\n");
	}

	// buffers
	uint8_t *rgba = malloc(RW * RH * 4);      // glReadPixels dst (RGBA8)
	uint16_t *rgb565 = malloc(RW * RH * 2);   // converted for 16bpp fb

	double t_render=0, t_read=0, t_conv=0, t_blit=0;

	for (int f = 0; f < frames; f++) {
		double t0 = now_ms();
		// --- render: animated clear color + a moving quad-ish via scissor ---
		float p = (f % 120) / 120.0f;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, RW, RH);
		glClearColor(p, 0.3f, 1.0f - p, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		glScissor((int)(p * (RW-40)), RH/2-20, 40, 40);
		glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glFinish();
		double t1 = now_ms();

		// --- readback ---
		glReadPixels(0, 0, RW, RH, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		double t2 = now_ms();

		// --- convert RGBA8 -> RGB565 ---
		for (int i = 0; i < RW*RH; i++) {
			uint8_t r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2];
			rgb565[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
		}
		double t3 = now_ms();

		// --- blit to fb0 (centered) - skipped if no display ---
		if (have_fb) {
			int ox = (vinfo.xres - RW) / 2, oy = (vinfo.yres - RH) / 2;
			if (vinfo.bits_per_pixel == 16) {
				for (int y = 0; y < RH; y++) {
					uint8_t *dst = fbp + (oy+y)*finfo.line_length + ox*2;
					memcpy(dst, rgb565 + y*RW, RW*2);
				}
			} else if (vinfo.bits_per_pixel == 32) {
				for (int y = 0; y < RH; y++) {
					uint32_t *dst = (uint32_t*)(fbp + (oy+y)*finfo.line_length) + ox;
					for (int x = 0; x < RW; x++) {
						uint8_t *s = rgba + (y*RW+x)*4;
						dst[x] = (s[0]<<16)|(s[1]<<8)|s[2];
					}
				}
			}
		}
		double t4 = now_ms();

		t_render += t1-t0; t_read += t2-t1; t_conv += t3-t2; t_blit += t4-t3;
	}

	printf("\n=== per-frame avg over %d frames (ms) ===\n", frames);
	printf("render(+glFinish): %.3f\n", t_render/frames);
	printf("glReadPixels:      %.3f\n", t_read/frames);
	printf("RGB565 convert:    %.3f\n", t_conv/frames);
	printf("fb0 blit(memcpy):  %.3f\n", t_blit/frames);
	double total = (t_render+t_read+t_conv+t_blit)/frames;
	printf("total:             %.3f  -> %.1f fps\n", total, 1000.0/total);

	if (have_fb) munmap(fbp, fbsize);
	if (fbfd >= 0) close(fbfd);
	eglDestroyContext(dpy, ctx); eglTerminate(dpy);
	gbm_device_destroy(gbm); close(drmfd);
	printf("OK\n");
	return 0;
}
