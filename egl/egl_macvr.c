// MacVR EGL shim for the emulator's vendor layer: forwards to ANGLE (libEGL_angle.so, a DT_NEEDED dependency,
// so the platform loader's dlsym finds every other egl* entry point there) and smooths over the few things
// the Quest runtime asks for that ANGLE on the emulator rejects.
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MacVR-EGL", __VA_ARGS__)

static void *real(const char *n) { return dlsym(RTLD_NEXT, n); }

// eglCreateContext: on EGL_BAD_ATTRIBUTE, drop the attributes ANGLE doesn't know (one at a time) and retry
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attr) {
    static EGLContext (*f)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    static EGLint (*err)(void);
    if (!f) { f = real("eglCreateContext"); err = real("eglGetError"); }
    EGLContext ctx = f(d, c, share, attr);
    if (ctx != EGL_NO_CONTEXT || !attr) return ctx;
    {   EGLint e = err(), rt = 0, id = 0, st = 0, r = 0, g = 0, b = 0, al = 0, dp = 0;
        EGLBoolean (*ga)(EGLDisplay, EGLConfig, EGLint, EGLint *) = real("eglGetConfigAttrib");
        ga(d, c, EGL_RENDERABLE_TYPE, &rt); ga(d, c, EGL_CONFIG_ID, &id); ga(d, c, EGL_SURFACE_TYPE, &st);
        ga(d, c, EGL_RED_SIZE, &r); ga(d, c, EGL_GREEN_SIZE, &g); ga(d, c, EGL_BLUE_SIZE, &b); ga(d, c, EGL_ALPHA_SIZE, &al); ga(d, c, EGL_DEPTH_SIZE, &dp);
        LOG("eglCreateContext failed 0x%x: config %p id %d renderable 0x%x surface 0x%x rgba %d%d%d%d depth %d share %p", e, c, id, rt, st, r, g, b, al, dp, share);
        for (int i = 0; attr[i] != EGL_NONE && i < 62; i += 2) LOG("  attr 0x%x=0x%x", attr[i], attr[i + 1]); }
    EGLint a[64]; int n = 0;
    for (; attr[n] != EGL_NONE && n < 62; n += 2) { a[n] = attr[n]; a[n + 1] = attr[n + 1]; }
    a[n] = EGL_NONE;
    for (int i = n - 2; i >= 0 && ctx == EGL_NO_CONTEXT; i -= 2) {
        EGLint k = a[i];
        if (k == EGL_CONTEXT_MAJOR_VERSION_KHR || k == EGL_CONTEXT_MINOR_VERSION_KHR || k == EGL_CONTEXT_CLIENT_VERSION) continue;
        LOG("eglCreateContext: dropping attribute 0x%x=0x%x", k, a[i + 1]);
        memmove(a + i, a + i + 2, (n - i) * sizeof(EGLint)); n -= 2;
        err(); ctx = f(d, c, share, a); if (ctx == EGL_NO_CONTEXT) LOG("  -> 0x%x", err());
    }
    // then step the requested GLES minor version down (3.2 -> 3.1 -> 3.0)
    for (int i = 0; i < n && ctx == EGL_NO_CONTEXT; i += 2)
        while (a[i] == EGL_CONTEXT_MINOR_VERSION_KHR && a[i + 1] > 0 && ctx == EGL_NO_CONTEXT) {
            a[i + 1]--; err(); ctx = f(d, c, share, a); LOG("eglCreateContext: minor version %d -> %s 0x%x", a[i + 1], ctx ? "ok" : "fail", err());
        }
    return ctx;
}

// eglCreatePbufferSurface / eglCreateWindowSurface: same treatment (protected content, colorspaces)
static EGLSurface retry_surface(EGLSurface (*f)(), EGLDisplay d, EGLConfig c, void *w, const EGLint *attr, int win) {
    EGLSurface s = win ? ((EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))f)(d, c, w, attr)
                       : ((EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *))f)(d, c, attr);
    if (s != EGL_NO_SURFACE || !attr) return s;
    EGLint a[64]; int n = 0;
    for (; attr[n] != EGL_NONE && n < 62; n += 2) { a[n] = attr[n]; a[n + 1] = attr[n + 1]; }
    a[n] = EGL_NONE;
    for (int i = n - 2; i >= 0 && s == EGL_NO_SURFACE; i -= 2) {
        if (a[i] == EGL_WIDTH || a[i] == EGL_HEIGHT) continue;
        LOG("%s: dropping attribute 0x%x=0x%x", win ? "eglCreateWindowSurface" : "eglCreatePbufferSurface", a[i], a[i + 1]);
        memmove(a + i, a + i + 2, (n - i) * sizeof(EGLint)); n -= 2;
        s = win ? ((EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))f)(d, c, w, a)
                : ((EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *))f)(d, c, a);
    }
    return s;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attr) {
    static void *f; if (!f) f = real("eglCreatePbufferSurface");
    return retry_surface(f, d, c, 0, attr, 0);
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *attr) {
    static void *f; if (!f) f = real("eglCreateWindowSurface");
    return retry_surface(f, d, c, w, attr, 1);
}

// the platform loader resolves extension entry points through the driver's eglGetProcAddress
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    static __eglMustCastToProperFunctionPointerType (*f)(const char *);
    if (!f) f = real("eglGetProcAddress");
    if (!strcmp(name, "eglCreateContext")) return (__eglMustCastToProperFunctionPointerType)eglCreateContext;
    if (!strcmp(name, "eglCreatePbufferSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreatePbufferSurface;
    if (!strcmp(name, "eglCreateWindowSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreateWindowSurface;
    return f(name);
}
