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
        if (m) { if (m->id) for (uint32_t i = 0; i < m->rv.n; i++) AHardwareBuffer_release(m->rv.buf[i]); m->id = memory; m->rv = rv; }
        pthread_mutex_unlock(&vlock);
        if (rv.genFd >= 0) close(rv.genFd);
        if (!m) for (uint32_t i = 0; i < rv.n; i++) AHardwareBuffer_release(rv.buf[i]);
    }
    ((void (*)(GLuint, GLuint64, GLenum, GLint))gl("glImportMemoryFdEXT"))(memory, size, type, fd);
}
__attribute__((visibility("default"))) void macvr_DeleteMemoryObjects(GLsizei n, const GLuint *ids) {
    pthread_mutex_lock(&vlock);
    for (GLsizei i = 0; i < n; i++) { MemObj *m = memObj(ids[i]); if (m) { for (uint32_t k = 0; k < m->rv.n; k++) AHardwareBuffer_release(m->rv.buf[k]); memset(m, 0, sizeof *m); } }
    pthread_mutex_unlock(&vlock);
    ((void (*)(GLsizei, const GLuint *))gl("glDeleteMemoryObjectsEXT"))(n, ids);
}
__attribute__((visibility("default"))) void macvr_TexStorageMem2D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLuint m, GLuint64 o) {
    noteTex(boundTex(t), m);
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem2DEXT"))(t, l, f, w, h, m, o);
}
__attribute__((visibility("default"))) void macvr_TexStorageMem3D(GLenum t, GLsizei l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLuint m, GLuint64 o) {
    noteTex(boundTex(t), m);
    ((void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLuint, GLuint64))gl("glTexStorageMem3DEXT"))(t, l, f, w, h, d, m, o);
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
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, view);
    ((void (*)(GLenum, GLeglImageOES))gl("glEGLImageTargetTexture2DOES"))(tg, img);
    ((void (*)(GLenum, GLuint))gl("glBindTexture"))(tg, prev);
    // the texture keeps the image's storage alive
    ((EGLBoolean (*)(EGLDisplay, EGLImageKHR))real("eglDestroyImageKHR"))(dpy, img);
}

// the platform loader resolves extension entry points through the driver's eglGetProcAddress
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    static __eglMustCastToProperFunctionPointerType (*f)(const char *);
    if (!f) f = real("eglGetProcAddress");
    if (!strcmp(name, "eglCreateContext")) return (__eglMustCastToProperFunctionPointerType)eglCreateContext;
    if (!strcmp(name, "eglCreatePbufferSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreatePbufferSurface;
    if (!strcmp(name, "eglCreateWindowSurface")) return (__eglMustCastToProperFunctionPointerType)eglCreateWindowSurface;
    if (!strcmp(name, "glTextureViewOES") || !strcmp(name, "glTextureViewEXT")) return (__eglMustCastToProperFunctionPointerType)macvr_TextureView;
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
