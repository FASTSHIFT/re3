// SPI color-order test: render known colors to an RGB565 FBO exactly like re3's
// GBM path (glReadPixels GL_RGB/UNSIGNED_SHORT_5_6_5), then push to the ST7789
// via the same st7789_flush. Fills the panel with labeled solid quads so the
// physical panel colors reveal the byte/channel order to fix.
//
// Layout (320x240), 6 vertical bands left->right:
//   RED  GREEN  BLUE  WHITE  GRAY(128)  YELLOW(R+G)
//
// Build (cross): arm-linux-gnueabihf-gcc --sysroot=$SR -O2 spike/spi_colortest.c \
//   drivers/st7789/st7789.c drivers/st7789/pi_gpio.c -Idrivers/st7789 \
//   -o spi_colortest -lgbm -lEGL -lGLESv2
// Run: RE3_SPI_ENDIAN / RE3_SPI_BGR via args below.

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

#include "st7789.h"

#define RW 320
#define RH 240

#define CHECK(c,m) do{ if(!(c)){ fprintf(stderr,"FAIL: %s (gl=0x%x)\n",m,glGetError()); return 2; } }while(0)

static GLuint compile(GLenum t, const char* s){
	GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,0); glCompileShader(sh);
	GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
	if(!ok){char l[512];glGetShaderInfoLog(sh,512,0,l);fprintf(stderr,"shader:%s\n",l);}
	return sh;
}
static const char* VS="attribute vec2 p;attribute vec3 c;varying vec3 vc;void main(){vc=c;gl_Position=vec4(p,0.,1.);}";
static const char* FS="precision mediump float;varying vec3 vc;void main(){gl_FragColor=vec4(vc,1.);}";

int main(int argc, char** argv){
	// endian/bgr overridable via env to match re3 knobs
	int endian = getenv("RE3_SPI_ENDIAN") ? atoi(getenv("RE3_SPI_ENDIAN")) : 1;
	int bgr    = getenv("RE3_SPI_BGR")    ? atoi(getenv("RE3_SPI_BGR"))    : 0;
	uint32_t hz = getenv("RE3_SPI_HZ")    ? (uint32_t)strtoul(getenv("RE3_SPI_HZ"),0,10) : 70000000u;

	int drmfd=open("/dev/dri/renderD128",O_RDWR); CHECK(drmfd>=0,"open renderD128");
	struct gbm_device* gbm=gbm_create_device(drmfd); CHECK(gbm,"gbm_create_device");
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy=gpd?gpd(EGL_PLATFORM_GBM_KHR,gbm,0):eglGetDisplay((EGLNativeDisplayType)gbm);
	CHECK(dpy!=EGL_NO_DISPLAY,"getdisplay"); EGLint mj,mn; CHECK(eglInitialize(dpy,&mj,&mn),"init");
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
	EGLConfig cf; EGLint nc; if(!(eglChooseConfig(dpy,ca,&cf,1,&nc)&&nc>0)) cf=EGL_NO_CONFIG_KHR;
	EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
	EGLContext ctx=eglCreateContext(dpy,cf,EGL_NO_CONTEXT,xa); CHECK(ctx!=EGL_NO_CONTEXT,"ctx");
	CHECK(eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx),"makecurrent");

	// RGB565 FBO (same as librw GBM camera)
	GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,RW,RH,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,0);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	GLuint fbo; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
	CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"fbo");
	glViewport(0,0,RW,RH);

	GLuint pr=glCreateProgram();
	glAttachShader(pr,compile(GL_VERTEX_SHADER,VS));
	glAttachShader(pr,compile(GL_FRAGMENT_SHADER,FS));
	glBindAttribLocation(pr,0,"p"); glBindAttribLocation(pr,1,"c");
	glLinkProgram(pr); glUseProgram(pr);

	// 6 bands, each a full-height quad. colors: RED GREEN BLUE WHITE GRAY YELLOW
	float cols[6][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,1},{0.5,0.5,0.5},{1,1,0}};
	const char* names[6]={"RED","GREEN","BLUE","WHITE","GRAY","YELLOW"};
	glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
	for(int i=0;i<6;i++){
		float x0=-1.f + 2.f*i/6.f, x1=-1.f + 2.f*(i+1)/6.f;
		float v[]={ x0,-1, x1,-1, x0,1, x1,1 };
		float c[]={ cols[i][0],cols[i][1],cols[i][2], cols[i][0],cols[i][1],cols[i][2],
		            cols[i][0],cols[i][1],cols[i][2], cols[i][0],cols[i][1],cols[i][2] };
		glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,v); glEnableVertexAttribArray(0);
		glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,0,c); glEnableVertexAttribArray(1);
		glDrawArrays(GL_TRIANGLE_STRIP,0,4);
	}
	glFinish();

	uint16_t* buf=malloc(RW*RH*2);
	glReadPixels(0,0,RW,RH,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,buf);
	// report the raw 565 word at the center of each band (host little-endian)
	printf("endian=%d bgr=%d hz=%u\n", endian, bgr, hz);
	for(int i=0;i<6;i++){
		int cx=(int)((i+0.5f)*RW/6), cy=RH/2;
		uint16_t p=buf[cy*RW+cx];
		printf("band %d %-6s: 565=0x%04X  R=%d G=%d B=%d\n", i, names[i], p,
			(p>>11)&0x1F, (p>>5)&0x3F, p&0x1F);
	}

	// optional BGR swap in 565 space (same as sink_spi)
	if(bgr){
		for(int i=0;i<RW*RH;i++){uint16_t p=buf[i];uint16_t r=(p>>11)&0x1F,g=(p>>5)&0x3F,b=p&0x1F;buf[i]=(b<<11)|(g<<5)|r;}
	}

	// push via the same driver
	st7789_config_t cfg; st7789_config_default(&cfg);
	cfg.spi_hz=hz; cfg.little_endian=endian; cfg.invert=1;
	st7789_t* d=st7789_open(&cfg);
	if(!d){ fprintf(stderr,"st7789_open failed\n"); return 3; }
	// GL buffer is bottom-up; flip to top-down for the panel
	uint16_t* fb=malloc(RW*RH*2);
	for(int y=0;y<RH;y++) memcpy(fb+y*RW, buf+(RH-1-y)*RW, RW*2);
	for(int k=0;k<300;k++){ st7789_flush(d,fb); usleep(16000); }
	st7789_close(d);
	free(buf); free(fb);
	printf("done: pushed color bands for 5s\n");
	return 0;
}
