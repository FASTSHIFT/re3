/* egl_ext_query.c - query EGL extensions using GBM platform */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    int fd = open("/dev/dri/renderD128", 2);
    if (fd < 0) { perror("open renderD128"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(dpy, NULL, NULL);
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!ext) { puts("no extensions"); return 1; }
    char buf[32768];
    strncpy(buf, ext, sizeof(buf)-1);
    for (char *p = strtok(buf, " "); p; p = strtok(NULL, " "))
        puts(p);
    return 0;
}
