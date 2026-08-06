// Byte-stream verification: renders a solid red frame to an RGB565 FBO (exactly
// like re3's GBM camera), reads it back with glReadPixels, shows the raw bytes
// and the uint16 values, then pushes to the SPI panel so the user can see the
// actual color. If the panel shows red, the pipeline is correct. If not, print
// tells us exactly which byte went wrong.
//
// Build: arm-linux-gnueabihf-gcc --sysroot=$SR -O2 spike/spi_bytecheck.c \
//   drivers/st7789/st7789.c drivers/st7789/pi_gpio.c -Idrivers/st7789 \
//   -I$SR/usr/include -L$SR/usr/lib/arm-linux-gnueabihf \
//   -Wl,-rpath-link,$SR/usr/lib/arm-linux-gnueabihf \
//   -o spi_bytecheck -lgbm -lEGL -lGLESv2

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include "st7789.h"

#ifndef GL_DEPTH_COMPONENT24
#ifdef  GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24 GL_DEPTH_COMPONENT24_OES
#else
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#endif

#define RW 320
#define RH 240
#define CHK(c,m) do{if(!(c)){fprintf(stderr,"FAIL: %s\n",m);return 2;}}while(0)

int main(void) {
    // EGL+GBM setup (same as librw re3 path)
    int dfd = open("/dev/dri/renderD128", O_RDWR); CHK(dfd>=0,"renderD128");
    struct gbm_device *gbm = gbm_create_device(dfd); CHK(gbm,"gbm");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = gpd ? gpd(EGL_PLATFORM_GBM_KHR,gbm,NULL) : eglGetDisplay((EGLNativeDisplayType)gbm);
    CHK(dpy!=EGL_NO_DISPLAY,"dpy"); EGLint mj,mn; CHK(eglInitialize(dpy,&mj,&mn),"init");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig cf; EGLint nc;
    if(!(eglChooseConfig(dpy,ca,&cf,1,&nc)&&nc>0)) cf=EGL_NO_CONFIG_KHR;
    EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cf,EGL_NO_CONTEXT,xa); CHK(ctx!=EGL_NO_CONTEXT,"ctx");
    CHK(eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx),"current");
    printf("GL: %s\n", glGetString(GL_RENDERER));

    // RGB565 texture FBO + depth (same as librw rasterCreateCamera for GBM)
    GLuint tex,fbo,depth;
    glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,RW,RH,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glGenRenderbuffers(1,&depth); glBindRenderbuffer(GL_RENDERBUFFER,depth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,RW,RH);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    CHK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"fbo");
    glViewport(0,0,RW,RH);

    struct { const char *name; float r,g,b; uint16_t expected565; } tests[] = {
        {"RED",    1,0,0, 0xF800},
        {"GREEN",  0,1,0, 0x07E0},
        {"BLUE",   0,0,1, 0x001F},
        {"YELLOW", 1,1,0, 0xFFE0},
        {"GRAY",   0.5,0.5,0.5, 0x8410},
    };
    int ntests = sizeof(tests)/sizeof(tests[0]);

    uint16_t *buf = malloc(RW*RH*2);

    // SPI setup (identical to sink_spi.cpp defaults)
    st7789_config_t cfg; st7789_config_default(&cfg);
    {
        const char *e;
        if ((e=getenv("RE3_SPI_ENDIAN"))) cfg.little_endian=atoi(e);
        if ((e=getenv("RE3_SPI_HZ")))     cfg.spi_hz=(uint32_t)strtoul(e,0,10);
        if ((e=getenv("RE3_SPI_INVERT"))) cfg.invert=atoi(e);
    }
    printf("SPI endian=%d hz=%u invert=%d\n", cfg.little_endian, cfg.spi_hz, cfg.invert);
    st7789_t *dev = st7789_open(&cfg);
    CHK(dev,"st7789_open");

    for (int t = 0; t < ntests; t++) {
        // Render solid color
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glClearColor(tests[t].r, tests[t].g, tests[t].b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();

        // Readback via glReadPixels (same as _psPresent)
        glReadPixels(0,0,RW,RH,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,buf);

        // Inspect center pixel bytes
        uint16_t px = buf[(RH/2)*RW + RW/2];
        uint8_t *raw = (uint8_t*)&px;

        printf("\n[%s] expected 565=0x%04X  got 565=0x%04X  bytes=[0x%02X,0x%02X]  %s\n",
            tests[t].name, tests[t].expected565, px, raw[0], raw[1],
            px==tests[t].expected565 ? "MATCH" : "MISMATCH");
        printf("  R=%d G=%d B=%d (expected R=%d G=%d B=%d)\n",
            (px>>11)&0x1F, (px>>5)&0x3F, px&0x1F,
            (int)(tests[t].r*31+0.5f), (int)(tests[t].g*63+0.5f), (int)(tests[t].b*31+0.5f));
        printf("  SPI bytes sent (low_byte_first=endian%d): 0x%02X 0x%02X\n",
            cfg.little_endian, cfg.little_endian?raw[0]:raw[1], cfg.little_endian?raw[1]:raw[0]);

        // Push to panel (with vertical flip, same as sink_spi fast path)
        uint16_t *fb = malloc(RW*RH*2);
        for(int y=0;y<RH;y++) memcpy(fb+y*RW, buf+(RH-1-y)*RW, RW*2);
        st7789_flush(dev, fb);
        free(fb);

        printf("  -> pushing to panel for 4s, you should see: %s\n", tests[t].name);
        sleep(4);
    }

    st7789_close(dev);
    free(buf);
    printf("\nDone. If all colors matched on screen, pipeline is correct.\n");
    return 0;
}
