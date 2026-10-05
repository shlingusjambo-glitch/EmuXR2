#define _GNU_SOURCE
// MacVR EGL shim for the emulator's vendor layer: forwards to ANGLE (libEGL_angle.so, a DT_NEEDED dependency,
// so the platform loader's dlsym finds every other egl* entry point there) and smooths over the few things
// the Quest runtime asks for that ANGLE on the emulator rejects.
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
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
// window surfaces, so a switch to front-buffer rendering can turn on the window's auto-refresh: the Quest compositor
// draws straight into the front buffer, which its display scans out; on the emulator the buffer goes through
// SurfaceFlinger, which has to be told to show it again every vsync
static struct { EGLSurface s; EGLNativeWindowType w; int front; } wins[32];
static void hookRuntime(void);
static int isCompositor(void) {
    static int v = -1;
    if (v < 0) { char n[64] = {0}; FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { fread(n, 1, 63, f); fclose(f); } v = !strcmp(n, "com.oculus.vrruntimeservice"); }
    return v;
}
// present the compositor's surface (at most every 11 ms) when it makes a fence: its frame (or eye) is drawn
static void present(void) {
    if (!isCompositor()) return;
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW); int front = 0;
    for (int i = 0; i < 32; i++) if (wins[i].s == s && wins[i].front) front = 1;
    if (!front) return;
    static __thread struct timespec last; struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - last.tv_sec) * 1000000000LL + now.tv_nsec - last.tv_nsec < 11000000) return;
    last = now;
    ((EGLBoolean (*)(EGLDisplay, EGLSurface))real("eglSwapBuffers"))(eglGetCurrentDisplay(), s);
}
static void frontBuffer(EGLSurface s) {
    for (int i = 0; i < 32; i++) if (wins[i].s == s) {
        int (*ar)(EGLNativeWindowType, _Bool) = dlsym(RTLD_DEFAULT, "ANativeWindow_setAutoRefresh");
        LOG("front-buffer surface %p: auto-refresh %d", s, ar ? ar(wins[i].w, 1) : -1);
    }
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *attr) {
    static void *f; if (!f) f = real("eglCreateWindowSurface");
    EGLSurface s = retry_surface(f, d, c, w, attr, 1);
    if (s != EGL_NO_SURFACE) for (int i = 0; i < 32; i++) if (!wins[i].s || wins[i].s == s) { wins[i].s = s; wins[i].w = w; break; }
    for (int i = 0; s != EGL_NO_SURFACE && attr && attr[i] != EGL_NONE; i += 2)
        if (attr[i] == EGL_RENDER_BUFFER && attr[i + 1] == EGL_SINGLE_BUFFER) frontBuffer(s);
    LOG("eglCreateWindowSurface(%p) -> %p (compositor %d)", w, s, isCompositor());
    if (s != EGL_NO_SURFACE && isCompositor()) {
        hookRuntime();
        // Meta's compositor swaps once, then keeps drawing into that buffer, which the Quest's display scans out.
        // Here the surface keeps its contents across swaps, and is presented at the compositor's fences (see present).
        EGLBoolean ok = ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint))real("eglSurfaceAttrib"))(d, s, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        for (int i = 0; i < 32; i++) if (wins[i].s == s) wins[i].front = 1;
        LOG("compositor surface %p: preserved %d", s, ok);
    }
    return s;
}

// GL_EXT_memory_object's direct-state-access entry points (glTextureStorageMem*EXT), which ANGLE lacks and the
// Quest runtime uses to wrap imported swapchain memory: bind, call the non-DSA form, restore. The texture's
// target follows the function (a 3D call on a name already bound as GL_TEXTURE_3D falls back to that).
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
static void *gl(const char *n) {
    static __eglMustCastToProperFunctionPointerType (*f)(const char *);
    if (!f) f = real("eglGetProcAddress");
    return (void *)f(n);
}
static GLenum bindFor(GLuint tex, GLenum want, GLenum alt, GLint *prev) {
    void (*bind)(GLenum, GLuint) = gl("glBindTexture"); void (*geti)(GLenum, GLint *) = gl("glGetIntegerv"); GLenum (*e)(void) = gl("glGetError");
    GLenum targets[2] = {want, alt};
    for (int i = 0; i < 2 && targets[i]; i++) {
        GLenum q = targets[i] == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : targets[i] == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_BINDING_2D_ARRAY :
                   targets[i] == GL_TEXTURE_3D ? GL_TEXTURE_BINDING_3D : targets[i] == GL_TEXTURE_2D_MULTISAMPLE ? GL_TEXTURE_BINDING_2D_MULTISAMPLE :
                   GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
        geti(q, prev); while (e()) {}
        bind(targets[i], tex);
        if (e() == GL_NO_ERROR) return targets[i];
    }
    return 0;
}
static void noteTex(GLuint tex, GLuint mem);
static void TextureStorageMem2D(GLuint t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLuint m, GLuint64 o) {
    noteTex(t, m); GLint prev; GLenum tg = bindFor(t, GL_TEXTURE_2D, 0, &prev); if (!tg) return;
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem2DEXT"))(tg, l, f, w, h, m, o);
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
}
static void TextureStorageMem3D(GLuint t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLuint m, GLuint64 o) {
    noteTex(t, m); GLint prev; GLenum tg = bindFor(t, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D, &prev); if (!tg) return;
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem3DEXT"))(tg, l, f, w, h, d, m, o);
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
}
static void TextureStorageMem2DMS(GLuint t, GLsizei s, GLenum f, GLsizei w, GLsizei h, GLboolean fx, GLuint m, GLuint64 o) {
    GLint prev; GLenum tg = bindFor(t, GL_TEXTURE_2D_MULTISAMPLE, 0, &prev); if (!tg) return;
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean, GLuint, GLuint64))gl("glTexStorageMem2DMultisampleEXT"))(tg, s, f, w, h, fx, m, o);
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
}
static void TextureStorageMem3DMS(GLuint t, GLsizei s, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLboolean fx, GLuint m, GLuint64 o) {
    GLint prev; GLenum tg = bindFor(t, GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 0, &prev); if (!tg) return;
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean, GLuint, GLuint64))gl("glTexStorageMem3DMultisampleEXT"))(tg, s, f, w, h, d, fx, m, o);
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
}

// ---- GL_OES_texture_view, for the runtime ----
// ANGLE here has no texture views. The runtime only asks for a view of one layer and mip of a swapchain texture
// (often reinterpreted as sRGB), and every swapchain texture it has comes from memory shared by the Vulkan
// wrapper, which shares each layer and mip as its own AHardwareBuffer: so a view becomes an EGLImage of that
// buffer (sRGB colorspace when asked). glImportMemoryFdEXT and glTexStorageMem* are tracked to find it.
#include <EGL/eglext.h>
#include <pthread.h>
#include "../vk/fdmsg.h"
typedef struct { GLuint id; Recv rv; } MemObj;
typedef struct { GLuint tex, mem; } TexMem;
static MemObj mems[256]; static TexMem texs[1024];
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static MemObj *memObj(GLuint id) { for (int i = 0; i < 256; i++) if (mems[i].id == id) return &mems[i]; return NULL; }
static void noteTex(GLuint tex, GLuint mem) {
    LOG("texture %u <- memory %u", tex, mem);
    pthread_mutex_lock(&vlock);
    int k = -1;
    for (int i = 0; i < 1024; i++) { if (texs[i].tex == tex) { k = i; break; } if (k < 0 && !texs[i].tex) k = i; }
    if (k >= 0) texs[k] = (TexMem){tex, mem};
    pthread_mutex_unlock(&vlock);
}
static GLuint boundTex(GLenum target) {
    GLint t = 0; GLenum q = target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_BINDING_2D_ARRAY :
        target == GL_TEXTURE_3D ? GL_TEXTURE_BINDING_3D : target == GL_TEXTURE_2D_MULTISAMPLE ? GL_TEXTURE_BINDING_2D_MULTISAMPLE : GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
    ((void (*)(GLenum, GLint *))gl("glGetIntegerv"))(q, &t);
    return t;
}
__attribute__((visibility("default"))) void macvr_ImportMemoryFd(GLuint memory, GLuint64 size, GLenum type, GLint fd) {
    Recv rv; int got = !recvBuffers(fd, &rv);
    LOG("glImportMemoryFdEXT(mem %u, %llu bytes, fd %d): %u buffers", memory, (unsigned long long)size, fd, got ? rv.n : 0);
    if (got) {   // peeked, not consumed: ANGLE still imports the fd itself
        pthread_mutex_lock(&vlock);
        MemObj *m = memObj(memory); if (!m) m = memObj(0);
        if (m) { if (m->id) { for (uint32_t i = 0; i < m->rv.n; i++) AHardwareBuffer_release(m->rv.buf[i]); if (m->rv.genFd >= 0) close(m->rv.genFd); } m->id = memory; m->rv = rv; }
        pthread_mutex_unlock(&vlock);
        if (!m) { for (uint32_t i = 0; i < rv.n; i++) AHardwareBuffer_release(rv.buf[i]); if (rv.genFd >= 0) close(rv.genFd); }
    }
    ((void (*)(GLuint, GLuint64, GLenum, GLint))gl("glImportMemoryFdEXT"))(memory, size, type, fd);
}
__attribute__((visibility("default"))) void macvr_DeleteMemoryObjects(GLsizei n, const GLuint *ids) {
    pthread_mutex_lock(&vlock);
    for (GLsizei i = 0; i < n; i++) { MemObj *m = memObj(ids[i]); if (m) { for (uint32_t k = 0; k < m->rv.n; k++) AHardwareBuffer_release(m->rv.buf[k]); if (m->rv.genFd >= 0) close(m->rv.genFd); memset(m, 0, sizeof *m); } }
    pthread_mutex_unlock(&vlock);
    ((void (*)(GLsizei, const GLuint *))gl("glDeleteMemoryObjectsEXT"))(n, ids);
}
__attribute__((visibility("default"))) void macvr_TexStorageMem2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLuint m, GLuint64 o) {
    noteTex(boundTex(t), m);
    LOG("glTexStorageMem2DEXT(target 0x%x, %d levels, format 0x%x, %dx%d, memory %u)", t, l, f, w, h, m);
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem2DEXT"))(t, l, f, w, h, m, o);
}
// ANGLE accepts glTexStorageMem3DEXT but never allocates anything (only the 2D form is implemented), and every
// stereo swapchain Meta's apps render to is a 2-layer array shared from another process. Such a texture gets
// ordinary storage here; layers the app renders to are copied into the shared per-layer buffers before the app's
// next fence, and the shared generation counter moves once that copy is done, which is when the other side's
// Vulkan wrapper (vk_macvr.c) copies the layers in.
#include <poll.h>
#include <stdatomic.h>
#include <sys/mman.h>
#define MAXARR 64
typedef struct { GLuint tex; EGLContext ctx; int dirty; uint32_t layers, w, h, mips; AHardwareBuffer *buf[MAXSH]; GLuint layerTex[MAXSH];
                 volatile atomic_uint *gen; } Arr;
static Arr arrs[MAXARR];
static void *genWorker(void *u) {   // fd of a native fence, then the counter to bump once it signals
    int p = (int)(intptr_t)u;
    for (;;) {
        struct { int fd; volatile atomic_uint *gen[MAXARR]; int n; } j;
        if (read(p, &j, sizeof j) != sizeof j) continue;
        struct pollfd pf = {j.fd, POLLIN, 0};
        if (j.fd >= 0) { poll(&pf, 1, 1000); close(j.fd); }
        for (int i = 0; i < j.n; i++) atomic_fetch_add(j.gen[i], 1);
    }
    return NULL;
}
__attribute__((visibility("default"))) void macvr_TexStorageMem3D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLuint m, GLuint64 o) {
    GLuint tex = boundTex(t);
    noteTex(tex, m);
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem3DEXT"))(t, l, f, w, h, d, m, o);
    GLint have = 0;
    ((void (*)(GLenum, GLint, GLenum, GLint *))gl("glGetTexLevelParameteriv"))(t, 0, GL_TEXTURE_WIDTH, &have);
    if (have || t != GL_TEXTURE_2D_ARRAY) return;   // the driver did it (or it isn't a stereo array)
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))gl("glTexStorage3D"))(t, l, f, w, h, d);
    pthread_mutex_lock(&vlock);
    MemObj *mo = memObj(m); Arr *a = NULL;
    for (int i = 0; i < MAXARR && !a; i++) if (arrs[i].tex == tex && arrs[i].ctx == eglGetCurrentContext()) a = &arrs[i];
    for (int i = 0; i < MAXARR && !a; i++) if (!arrs[i].tex) a = &arrs[i];
    int depth = f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH_COMPONENT32F || f == GL_DEPTH24_STENCIL8 ||
                f == GL_DEPTH32F_STENCIL8 || f == GL_STENCIL_INDEX8;
    if (depth) a = NULL;   // depth layers aren't shown, and the host can't wrap a depth buffer as a color image
    if (a && mo && mo->rv.n >= (uint32_t)d) {
        for (uint32_t i = 0; i < MAXSH; i++) if (a->buf[i]) AHardwareBuffer_release(a->buf[i]);
        if (a->gen) munmap((void *)a->gen, 4096);
        memset(a, 0, sizeof *a);
        a->tex = tex; a->ctx = eglGetCurrentContext(); a->layers = d; a->w = w; a->h = h; a->mips = mo->rv.mips ? mo->rv.mips : 1;
        for (uint32_t i = 0; i < mo->rv.n && i < MAXSH; i++) { a->buf[i] = mo->rv.buf[i]; AHardwareBuffer_acquire(a->buf[i]); }
        if (mo->rv.genFd >= 0) { void *g = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mo->rv.genFd, 0); if (g != MAP_FAILED) a->gen = g; }
        LOG("texture %u: %dx%dx%d array over %u shared layers%s", tex, w, h, d, mo->rv.n, a->gen ? "" : " (no counter)");
    } else LOG("texture %u: array storage only (format 0x%x, memory %u has %u buffers)", tex, f, m, mo ? mo->rv.n : 0);
    pthread_mutex_unlock(&vlock);
}
// a framebuffer attachment of one of those arrays: its layers will need copying out
__attribute__((visibility("default"))) void macvr_NoteAttach(GLuint tex) {
    if (!tex) return;
    pthread_mutex_lock(&vlock);
    for (int i = 0; i < MAXARR; i++) if (arrs[i].tex == tex && arrs[i].ctx == eglGetCurrentContext()) arrs[i].dirty = 1;
    pthread_mutex_unlock(&vlock);
}
// copy rendered arrays into their shared layers, ahead of the fence the app is about to make
__attribute__((visibility("default"))) void macvr_FlushArrays(void) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    present();
    struct { int fd; volatile atomic_uint *gen[MAXARR]; int n; } j = {-1, {0}, 0};
    pthread_mutex_lock(&vlock);
    for (int i = 0; i < MAXARR; i++) {
        Arr *a = &arrs[i];
        if (!a->tex || !a->dirty || a->ctx != ctx) continue;
        a->dirty = 0;
        for (uint32_t L = 0; L < a->layers; L++) {
            uint32_t k = L * a->mips;
            if (k >= MAXSH || !a->buf[k]) continue;
            if (!a->layerTex[L]) {   // a 2D texture over the layer's shared buffer (raw bits: linear colorspace)
                EGLDisplay dpy = eglGetCurrentDisplay();
                EGLClientBuffer cb = ((EGLClientBuffer (*)(const AHardwareBuffer *))gl("eglGetNativeClientBufferANDROID"))(a->buf[k]);
                EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
                EGLImageKHR img = ((EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))real("eglCreateImageKHR"))(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
                if (img == EGL_NO_IMAGE_KHR) { LOG("texture %u layer %u: no image", a->tex, L); continue; }
                GLint prev = boundTex(GL_TEXTURE_2D);
                ((void (*)(GLsizei, GLuint *))gl("glGenTextures"))(1, &a->layerTex[L]);
                ((void (*)(GLenum, GLuint))gl("glBindTexture"))(GL_TEXTURE_2D, a->layerTex[L]);
                ((void (*)(GLenum, GLeglImageOES))gl("glEGLImageTargetTexture2DOES"))(GL_TEXTURE_2D, img);
                ((void (*)(GLenum, GLuint))gl("glBindTexture"))(GL_TEXTURE_2D, prev);
                ((EGLBoolean (*)(EGLDisplay, EGLImageKHR))real("eglDestroyImageKHR"))(dpy, img);
            }
            ((void (*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei))gl("glCopyImageSubDataEXT"))(
                a->tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, L, a->layerTex[L], GL_TEXTURE_2D, 0, 0, 0, 0, a->w, a->h, 1);
        }
        if (a->gen && j.n < MAXARR) j.gen[j.n++] = a->gen;
    }
    pthread_mutex_unlock(&vlock);
    if (!j.n) return;
    static int pipeFd[2] = {-1, -1};
    if (pipeFd[0] < 0) {
        pthread_t th;
        if (pipe(pipeFd) || pthread_create(&th, NULL, genWorker, (void *)(intptr_t)pipeFd[0])) { LOG("no generation worker"); return; }
        pthread_detach(th);
    }
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSyncKHR sync = ((EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint *))real("eglCreateSyncKHR"))(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    ((void (*)(void))gl("glFlush"))();
    if (sync != EGL_NO_SYNC_KHR) {
        j.fd = ((EGLint (*)(EGLDisplay, EGLSyncKHR))real("eglDupNativeFenceFDANDROID"))(dpy, sync);
        ((EGLBoolean (*)(EGLDisplay, EGLSyncKHR))real("eglDestroySyncKHR"))(dpy, sync);
    }
    write(pipeFd[1], &j, sizeof j);
}
// the app's fences: its frame is done
EGLSyncKHR eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint *attr) {
    static EGLSyncKHR (*f)(EGLDisplay, EGLenum, const EGLint *); if (!f) f = real("eglCreateSyncKHR");
    macvr_FlushArrays();
    return f(d, type, attr);
}
EGLSync eglCreateSync(EGLDisplay d, EGLenum type, const EGLAttrib *attr) {
    static EGLSync (*f)(EGLDisplay, EGLenum, const EGLAttrib *); if (!f) f = real("eglCreateSync");
    macvr_FlushArrays();
    return f(d, type, attr);
}
static int isSrgb(GLenum f) { return f == GL_SRGB8_ALPHA8 || f == GL_SRGB8 || f == 0x8FBD /* GL_SR8_EXT */; }
__attribute__((visibility("default"))) void macvr_TextureView(GLuint view, GLenum target, GLuint orig, GLenum fmt, GLuint minlevel, GLuint numlevels, GLuint minlayer, GLuint numlayers) {
    AHardwareBuffer *buf = NULL;
    pthread_mutex_lock(&vlock);
    for (int i = 0; i < 1024; i++) if (texs[i].tex == orig) {
        MemObj *m = memObj(texs[i].mem);
        if (m && m->rv.n) {
            uint32_t mips = m->rv.mips ? m->rv.mips : 1, k = minlayer * mips + minlevel;
            if (k < m->rv.n) { buf = m->rv.buf[k]; AHardwareBuffer_acquire(buf); }
        }
        break;
    }
    pthread_mutex_unlock(&vlock);
    if (!buf || numlayers > 1 || numlevels > 1 || (target != GL_TEXTURE_2D && target != GL_TEXTURE_2D_ARRAY)) {
        LOG("glTextureViewOES(view %u target 0x%x orig %u fmt 0x%x level %u+%u layer %u+%u): unsupported%s", view, target, orig, fmt,
            minlevel, numlevels, minlayer, numlayers, buf ? "" : " (no shared buffer)");
        if (buf) AHardwareBuffer_release(buf);
        return;
    }
    EGLDisplay dpy = ((EGLDisplay (*)(void))real("eglGetCurrentDisplay"))();
    EGLClientBuffer cb = ((EGLClientBuffer (*)(const AHardwareBuffer *))gl("eglGetNativeClientBufferANDROID"))(buf);
    EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_GL_COLORSPACE_KHR, isSrgb(fmt) ? EGL_GL_COLORSPACE_SRGB_KHR : EGL_GL_COLORSPACE_LINEAR_KHR, EGL_NONE};
    EGLImageKHR img = ((EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))real("eglCreateImageKHR"))(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
    AHardwareBuffer_release(buf);
    if (img == EGL_NO_IMAGE_KHR) { LOG("glTextureViewOES: eglCreateImageKHR failed"); return; }
    GLenum tg = target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    GLint prev = boundTex(tg);
    while (((GLenum (*)(void))gl("glGetError"))() != GL_NO_ERROR) {}
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, view);
    GLenum bindErr = ((GLenum (*)(void))gl("glGetError"))();
    ((void (*)(GLenum, GLeglImageOES))gl("glEGLImageTargetTexture2DOES"))(tg, img);
    GLenum error = ((GLenum (*)(void))gl("glGetError"))();
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
    if (bindErr) LOG("texture view %u: bind error 0x%x", view, bindErr);
    LOG("texture view %u <- %u layer %u target 0x%x: GL error 0x%x", view, orig, minlayer, tg, error);
    // the texture keeps the image's storage alive
    ((EGLBoolean (*)(EGLDisplay, EGLImageKHR))real("eglDestroyImageKHR"))(dpy, img);
}

// the platform loader resolves extension entry points through the driver's eglGetProcAddress
EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface, EGLint, EGLint);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay, EGLSurface, EGLint *, EGLint);
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    static __eglMustCastToProperFunctionPointerType (*f)(const char *);
    if (!f) f = real("eglGetProcAddress");
    if (!strcmp(name, "eglCreateContext")) return (__eglMustCastToProperFunctionPointerType)eglCreateContext;
    if (!strcmp(name, "eglCreatePbufferSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreatePbufferSurface;
    if (!strcmp(name, "eglCreateWindowSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreateWindowSurface;
    if (!strcmp(name, "glTextureViewOES") || !strcmp(name, "glTextureViewEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_TextureView;
    if (!strcmp(name, "eglCreateSyncKHR")) return (__eglMustCastToProperFunctionPointerType)eglCreateSyncKHR;
    if (!strcmp(name, "eglSwapBuffers")) return (__eglMustCastToProperFunctionPointerType)eglSwapBuffers;
    if (!strcmp(name, "eglSwapBuffersWithDamageKHR")) return (__eglMustCastToProperFunctionPointerType)eglSwapBuffersWithDamageKHR;
    if (!strcmp(name, "eglSurfaceAttrib")) return (__eglMustCastToProperFunctionPointerType)eglSurfaceAttrib;
    if (!strcmp(name, "eglCreateSync")) return (__eglMustCastToProperFunctionPointerType)eglCreateSync;
    if (!strcmp(name, "glImportMemoryFdEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_ImportMemoryFd;
    if (!strcmp(name, "glDeleteMemoryObjectsEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_DeleteMemoryObjects;
    if (!strcmp(name, "glTexStorageMem2DEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_TexStorageMem2D;
    if (!strcmp(name, "glTexStorageMem3DEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_TexStorageMem3D;
    if (!strcmp(name, "glTextureStorageMem2DEXT")) return (__eglMustCastToProperFunctionPointerType)TextureStorageMem2D;
    if (!strcmp(name, "glTextureStorageMem3DEXT")) return (__eglMustCastToProperFunctionPointerType)TextureStorageMem3D;
    if (!strcmp(name, "glTextureStorageMem2DMultisampleEXT")) return (__eglMustCastToProperFunctionPointerType)TextureStorageMem2DMS;
    if (!strcmp(name, "glTextureStorageMem3DMultisampleEXT")) return (__eglMustCastToProperFunctionPointerType)TextureStorageMem3DMS;
    return f(name);
}

// ---- front-buffer compositor ----
EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint v) {
    static EGLBoolean (*f)(EGLDisplay, EGLSurface, EGLint, EGLint); if (!f) f = real("eglSurfaceAttrib");
    EGLBoolean r = f(d, s, attr, v);
    if (r && attr == EGL_RENDER_BUFFER && v == EGL_SINGLE_BUFFER) frontBuffer(s);
    return r;
}
static void dumpSwap(EGLDisplay d, EGLSurface s);
// swaps on window surfaces (logged occasionally)
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    static EGLBoolean (*f)(EGLDisplay, EGLSurface); if (!f) f = real("eglSwapBuffers");
    dumpSwap(d, s);
    EGLBoolean r = f(d, s);
    static unsigned n; if (n < 3 || n % 500 == 0) LOG("eglSwapBuffers(%p) #%u -> %d (0x%x)", s, n, r, ((EGLint (*)(void))real("eglGetError"))()); n++;
    return r;
}
// debug: debug.macvr.dumpswap=<process name> writes each window surface's frame to /data/local/tmp/swap-<pid>-<w>x<h>.rgba
#include <sys/system_properties.h>
static void dumpSwap(EGLDisplay d, EGLSurface s) {
    static int want = -1; char v[PROP_VALUE_MAX] = {0};
    if (want < 0) {
        char n[64] = {0}; FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { fread(n, 1, 63, f); fclose(f); }
        __system_property_get("debug.macvr.dumpswap", v); want = v[0] && !strcmp(v, n);
    }
    if (!want) return;
    EGLint w = 0, h = 0;
    ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint *))real("eglQuerySurface"))(d, s, EGL_WIDTH, &w);
    ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint *))real("eglQuerySurface"))(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || w * h > 16 << 20) return;
    void *px = malloc((size_t)w * h * 4);
    ((void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))gl("glReadPixels"))(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[128]; snprintf(path, sizeof path, "/data/local/tmp/swap-%d-%dx%d.rgba", getpid(), w, h);
    FILE *o = fopen(path, "wb"); if (o) { fwrite(px, 4, (size_t)w * h, o); fclose(o); }
    free(px);
}
EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n_) {
    static EGLBoolean (*f)(EGLDisplay, EGLSurface, EGLint *, EGLint); if (!f) f = real("eglSwapBuffersWithDamageKHR");
    dumpSwap(d, s);
    EGLBoolean r = f(d, s, rects, n_);
    static unsigned n; if (n < 3 || n % 500 == 0) LOG("eglSwapBuffersWithDamageKHR(%p) #%u -> %d", s, n, r); n++;
    return r;
}

// ---- Android-surface swapchain placeholder size (runtime process only) ----
// The runtime starts each Android-surface swapchain as an image reader of 65536x65536 until the producer's real
// size arrives, and SurfaceFlinger sizes the virtual display that feeds it from that placeholder. The Quest's gralloc
// just refuses such a buffer; the emulator's host GPU tries to allocate it and crashes. Readers above the host's
// limit start at 4096 instead (the display is resized to the panel's real size right after, as on the headset).
// The runtime's own import of AImageReader_newWithUsage is pointed here (a GOT entry; nothing else changes).
#include <link.h>
#include <media/NdkImageReader.h>
#include <sys/mman.h>
static media_status_t (*realNewReader)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
static int32_t clampDim(int32_t v) { return v > 16384 ? 4096 : v; }
static media_status_t newReader(int32_t w, int32_t h, int32_t fmt, uint64_t usage, int32_t maxImages, AImageReader **out) {
    if (clampDim(w) != w || clampDim(h) != h) LOG("image reader %dx%d -> %dx%d", w, h, clampDim(w), clampDim(h));
    return realNewReader(clampDim(w), clampDim(h), fmt, usage, maxImages, out);
}
// android::SurfaceTexture::setDefaultBufferSize(uint32_t, uint32_t), as called for Java SurfaceTextures (GLES swapchains)
static int32_t (*realStSize)(void *, uint32_t, uint32_t);
static int32_t stSize(void *self, uint32_t w, uint32_t h) {
    if (w > 16384 || h > 16384) LOG("surface texture %ux%u -> %dx%d", w, h, clampDim(w), clampDim(h));
    return realStSize(self, clampDim(w), clampDim(h));
}
static const struct { const char *lib, *sym; void *fn, **orig; } hooks[] = {
    {"libvrruntimeservice.so", "AImageReader_newWithUsage", (void *)newReader, (void **)&realNewReader},
    {"libandroid_runtime.so", "_ZN7android14SurfaceTexture20setDefaultBufferSizeEjj", (void *)stSize, (void **)&realStSize},
};
static int patchGot(struct dl_phdr_info *info, size_t size, void *data) {
    for (unsigned h = 0; h < sizeof hooks / sizeof *hooks; h++) {
        if (!info->dlpi_name || !strstr(info->dlpi_name, hooks[h].lib)) continue;
        ElfW(Dyn) *dyn = NULL;
        for (int i = 0; i < info->dlpi_phnum; i++) if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (void *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        if (!dyn) continue;
        ElfW(Sym) *sym = NULL; const char *str = NULL; ElfW(Rela) *rel = NULL; size_t relsz = 0;
        for (ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_SYMTAB) sym = (void *)(info->dlpi_addr + d->d_un.d_ptr);
            if (d->d_tag == DT_STRTAB) str = (void *)(info->dlpi_addr + d->d_un.d_ptr);
            if (d->d_tag == DT_JMPREL) rel = (void *)(info->dlpi_addr + d->d_un.d_ptr);
            if (d->d_tag == DT_PLTRELSZ) relsz = d->d_un.d_val;
        }
        for (size_t i = 0; rel && sym && str && i < relsz / sizeof *rel; i++) {
            if (strcmp(str + sym[ELF64_R_SYM(rel[i].r_info)].st_name, hooks[h].sym)) continue;
            void **got = (void **)(info->dlpi_addr + rel[i].r_offset);
            uintptr_t page = (uintptr_t)got & ~(uintptr_t)4095;
            if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE)) { LOG("hook %s: mprotect failed", hooks[h].sym); break; }
            *hooks[h].orig = *got; *got = hooks[h].fn;
            mprotect((void *)page, 4096, PROT_READ);
            LOG("hook %s in %s installed", hooks[h].sym, hooks[h].lib);
            break;
        }
    }
    return 0;
}
static void hookRuntime(void) { static int done; if (!done) { done = 1; dl_iterate_phdr(patchGot, NULL); } }
