// Fence round trip on the guest's GL stack: draw a cleared 1440x1584 pbuffer, fence, wait. Prints ms percentiles.
// Usage: fence_latency [n] [clears per frame]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void *a, const void *b) { double d = *(const double *)a - *(const double *)b; return (d > 0) - (d < 0); }

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200, clears = argc > 2 ? atoi(argv[2]) : 1;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d, NULL, NULL);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE}, nc;
    EGLConfig c; eglChooseConfig(d, ca, &c, 1, &nc);
    EGLint sa[] = {EGL_WIDTH, 1440, EGL_HEIGHT, 1584, EGL_NONE}, xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa); EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    double *ms = malloc(n * sizeof *ms);
    for (int i = 0; i < n; i++) {
        double t = now();
        for (int k = 0; k < clears; k++) { glClearColor(k & 1, i & 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
        EGLSync f = eglCreateSync(d, EGL_SYNC_FENCE, NULL);
        glFlush();
        eglClientWaitSync(d, f, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
        eglDestroySync(d, f);
        ms[i] = now() - t;
    }
    qsort(ms, n, sizeof *ms, cmp);
    printf("fence round trip ms (n %d, %d clears): p50 %.2f p90 %.2f p99 %.2f max %.2f\n", n, clears, ms[n / 2], ms[n * 9 / 10], ms[n * 99 / 100], ms[n - 1]);
    return 0;
}
