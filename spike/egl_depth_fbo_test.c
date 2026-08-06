// Minimal reproducer: does depth testing work when rendering into a
// texture-backed FBO with a DEPTH24_STENCIL8 renderbuffer, under EGL+GBM
// surfaceless GLES2 -- i.e. the EXACT config librw uses for the re3 GBM camera?
//
// This isolates "broken faces / z-order" to the platform GL stack (VC4 Mesa)
// vs the game: it draws two overlapping depth-tested triangles (near=RED at
// z=-0.5, far=GREEN at z=+0.5) and reads back the center pixel where they
// overlap. Correct depth => center is RED (near occludes far). If the center
// is GREEN (or garbage), depth testing in this FBO config is broken on this
// GL stack, which explains the z-fighting/broken-faces independent of GTA3.
//
// It renders BOTH orderings (near-first and far-first) so painter's-order can't
// accidentally give the right answer:
//   - pass A: draw FAR then NEAR  -> center RED iff depth works
//   - pass B: draw NEAR then FAR  -> center RED iff depth works (far is rejected)
// A correct depth buffer yields RED in both passes. No-depth yields whatever
// was drawn last (GREEN in B), proving depth is not functioning.
//
// Build (native on Pi): gcc -O2 egl_depth_fbo_test.c -o egl_depth_fbo_test -lgbm -lEGL -lGLESv2
// Run (desktop stopped): ./egl_depth_fbo_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// GLES2 core lacks these; they come from OES_packed_depth_stencil (VC4 has it,
// verified earlier). Fall back to the OES enum values if headers omit them.
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_OES
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif

#define RW 320
#define RH 240

#define CHECK(c,m) do{ if(!(c)){ fprintf(stderr,"FAIL: %s (gl=0x%x)\n",m,glGetError()); return 2; } }while(0)

static GLuint compile(GLenum type, const char* src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if(!ok){ char log[512]; glGetShaderInfoLog(s,512,NULL,log); fprintf(stderr,"shader: %s\n",log); }
	return s;
}

// vertex color passthrough; position includes z for depth
static const char* VS =
	"attribute vec3 aPos;\n"
	"attribute vec3 aCol;\n"
	"varying vec3 vCol;\n"
	"void main(){ vCol=aCol; gl_Position=vec4(aPos,1.0); }\n";
static const char* FS =
	"precision mediump float;\n"
	"varying vec3 vCol;\n"
	"void main(){ gl_FragColor=vec4(vCol,1.0); }\n";

// A big triangle covering the center, at depth z, given color.
static void drawTri(GLint posLoc, GLint colLoc, float z, float r, float g, float b) {
	float verts[] = {
		-0.8f,-0.8f, z,   r,g,b,
		 0.8f,-0.8f, z,   r,g,b,
		 0.0f, 0.8f, z,   r,g,b,
	};
	glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), verts);
	glVertexAttribPointer(colLoc, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), verts+3);
	glEnableVertexAttribArray(posLoc);
	glEnableVertexAttribArray(colLoc);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

static int centerColor(const char* tag) {
	uint8_t px[4];
	glReadPixels(RW/2, RH/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	const char* c = "OTHER";
	if(px[0]>180 && px[1]<80) c="RED (near) -> depth OK";
	else if(px[1]>180 && px[0]<80) c="GREEN (far) -> DEPTH BROKEN";
	printf("  %s: center=(%d,%d,%d) => %s\n", tag, px[0],px[1],px[2], c);
	return (px[0]>180 && px[1]<80) ? 1 : 0;  // 1 = correct (red)
}

int main(void) {
	int drmfd = open("/dev/dri/renderD128", O_RDWR);
	CHECK(drmfd>=0, "open renderD128");
	struct gbm_device* gbm = gbm_create_device(drmfd);
	CHECK(gbm, "gbm_create_device");

	PFNEGLGETPLATFORMDISPLAYEXTPROC getPD =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = getPD ? getPD(EGL_PLATFORM_GBM_KHR, gbm, NULL)
	                       : eglGetDisplay((EGLNativeDisplayType)gbm);
	CHECK(dpy!=EGL_NO_DISPLAY, "eglGetDisplay");
	EGLint mj,mn; CHECK(eglInitialize(dpy,&mj,&mn), "eglInitialize");
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint cfgattr[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
	EGLConfig cfg; EGLint nc;
	if(!(eglChooseConfig(dpy,cfgattr,&cfg,1,&nc)&&nc>0)) cfg=EGL_NO_CONFIG_KHR;
	EGLint ctxattr[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
	EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxattr);
	CHECK(ctx!=EGL_NO_CONTEXT,"eglCreateContext");
	CHECK(eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx),"eglMakeCurrent");
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));

	// Color texture (shared across depth-format trials).
	GLuint tex; glGenTextures(1,&tex);
	glBindTexture(GL_TEXTURE_2D,tex);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,RW,RH,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);

	glViewport(0,0,RW,RH);

	GLuint prog=glCreateProgram();
	glAttachShader(prog,compile(GL_VERTEX_SHADER,VS));
	glAttachShader(prog,compile(GL_FRAGMENT_SHADER,FS));
	glBindAttribLocation(prog,0,"aPos");
	glBindAttribLocation(prog,1,"aCol");
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint posLoc=glGetAttribLocation(prog,"aPos");
	GLint colLoc=glGetAttribLocation(prog,"aCol");
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);	// same as librw

	// Try several depth-attachment strategies; report which give working depth.
	struct { const char* name; GLenum ifmt; GLenum attach; } trials[] = {
		{ "DEPTH24_STENCIL8 -> DEPTH_STENCIL_ATTACHMENT (current librw)", GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT },
		{ "DEPTH_COMPONENT16 -> DEPTH_ATTACHMENT",                        GL_DEPTH_COMPONENT16, GL_DEPTH_ATTACHMENT },
#ifdef GL_DEPTH_COMPONENT24_OES
		{ "DEPTH_COMPONENT24_OES -> DEPTH_ATTACHMENT",                    GL_DEPTH_COMPONENT24_OES, GL_DEPTH_ATTACHMENT },
#endif
	};

	int anyGood = 0;
	for (int t = 0; t < (int)(sizeof(trials)/sizeof(trials[0])); t++) {
		printf("\n--- trial %d: %s ---\n", t, trials[t].name);

		GLuint depth; glGenRenderbuffers(1,&depth);
		glBindRenderbuffer(GL_RENDERBUFFER,depth);
		glRenderbufferStorage(GL_RENDERBUFFER, trials[t].ifmt, RW, RH);
		GLenum rbErr = glGetError();

		GLuint fbo; glGenFramebuffers(1,&fbo);
		glBindFramebuffer(GL_FRAMEBUFFER,fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, trials[t].attach, GL_RENDERBUFFER, depth);
		GLenum st=glCheckFramebufferStatus(GL_FRAMEBUFFER);
		GLint bits=0; glGetIntegerv(GL_DEPTH_BITS,&bits);
		printf("  rbStorage err=0x%x  FBO status=0x%x (COMPLETE=0x%x)  GL_DEPTH_BITS=%d\n",
			rbErr, st, GL_FRAMEBUFFER_COMPLETE, bits);

		if (st == GL_FRAMEBUFFER_COMPLETE) {
			glClearColor(0,0,0,1); glClearDepthf(1.0f);
			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
			drawTri(posLoc,colLoc, 0.5f, 0,1,0);
			drawTri(posLoc,colLoc,-0.5f, 1,0,0);
			glFinish();
			int okA=centerColor("A far-then-near");

			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
			drawTri(posLoc,colLoc,-0.5f, 1,0,0);
			drawTri(posLoc,colLoc, 0.5f, 0,1,0);
			glFinish();
			int okB=centerColor("B near-then-far");

			printf("  => %s\n", (okA&&okB) ? "DEPTH OK" : "DEPTH BROKEN");
			if (okA&&okB) anyGood = 1;
		}

		glDeleteFramebuffers(1,&fbo);
		glDeleteRenderbuffers(1,&depth);
	}

	printf("\n=== VERDICT ===\n");
	printf("%s\n", anyGood ? "At least one depth format works on this GL stack." :
	                          "NO depth format worked -> deeper platform issue.");

	eglDestroyContext(dpy,ctx); eglTerminate(dpy);
	gbm_device_destroy(gbm); close(drmfd);
	return anyGood?0:1;
}
