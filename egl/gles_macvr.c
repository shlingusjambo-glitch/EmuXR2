// MacVR GLES v2/v3 shim over ANGLE (a DT_NEEDED dependency, so every other gl* entry point resolves there).
// ANGLE on the emulator's Vulkan tops out at ES 3.1; Meta's shaders declare 320 es but (so far) use 3.1
// features, so the version line is rewritten.
#include <GLES3/gl31.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MacVR-GLES", __VA_ARGS__)

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    static void (*f)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    if (!f) f = dlsym(RTLD_NEXT, "glShaderSource");
    size_t n = 0;
    for (GLsizei i = 0; i < count; i++) n += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
    char *s = malloc(n + 1), *p = s;
    for (GLsizei i = 0; i < count; i++) {
        size_t k = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
        memcpy(p, string[i], k); p += k;
    }
    *p = 0;
    char *v = strstr(s, "#version 320 es");
    if (v) memcpy(v, "#version 310 es", 15);
    const GLchar *one = s;
    f(shader, 1, &one, NULL);
    free(s);
}

// GL_OES_texture_view is emulated (in egl_macvr.c) for the Quest runtime only, which calls it even when absent
static int wantsViews(void) {
    static int v = -1;
    if (v < 0) { char n[64] = {0}; FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { fread(n, 1, 63, f); fclose(f); } v = !strcmp(n, "com.oculus.vrruntimeservice"); }
    return v;
}
static const char *hidden[] = {"GL_EXT_texture_view"};
const GLubyte *glGetString(GLenum name) {
    static const GLubyte *(*f)(GLenum);
    static __thread char *cache; static __thread const GLubyte *src;
    if (!f) f = dlsym(RTLD_NEXT, "glGetString");
    const GLubyte *s = f(name);
    if (name != GL_EXTENSIONS || !s) return s;
    if (s == src && cache) return (const GLubyte *)cache;
    free(cache); cache = malloc(strlen((const char *)s) + 32); strcpy(cache, (const char *)s); src = s;
    if (wantsViews() && !strstr(cache, "GL_OES_texture_view")) { strcat(cache, " GL_OES_texture_view"); LOG("advertising GL_OES_texture_view"); }
    for (unsigned i = 0; i < sizeof hidden / sizeof *hidden; i++) {
        char *p = strstr(cache, hidden[i]);
        if (p) { size_t n = strlen(hidden[i]); memmove(p, p + n + (p[n] == ' '), strlen(p + n + (p[n] == ' ')) + 1); LOG("hiding %s", hidden[i]); }
    }
    return (const GLubyte *)cache;
}
const GLubyte *glGetStringi(GLenum name, GLuint index) {
    static const GLubyte *(*f)(GLenum, GLuint);
    if (!f) f = dlsym(RTLD_NEXT, "glGetStringi");
    const GLubyte *s = f(name, index);
    if (name == GL_EXTENSIONS && s)
        for (unsigned i = 0; i < sizeof hidden / sizeof *hidden; i++) if (!strcmp((const char *)s, hidden[i])) return (const GLubyte *)"GL_MACVR_hidden";
    return s;
}

// ---- entry points the platform loader resolves here by name: forwarded to the texture-view tracking in libEGL_macvr ----
static void *eglShim(const char *n) {
    static void *h;
    if (!h) h = dlopen("libEGL_macvr.so", RTLD_NOW | RTLD_NOLOAD);
    return h ? dlsym(h, n) : NULL;
}
#define FWD(gl, ours, params, args) void gl params { static void (*f) params; if (!f) f = eglShim(ours); f args; }
FWD(glImportMemoryFdEXT, "macvr_ImportMemoryFd", (GLuint m, GLuint64 s, GLenum t, GLint fd), (m, s, t, fd))
FWD(glDeleteMemoryObjectsEXT, "macvr_DeleteMemoryObjects", (GLsizei n, const GLuint *ids), (n, ids))
FWD(glTexStorageMem2DEXT, "macvr_TexStorageMem2D", (GLenum t, GLsizei l, GLenum f_, GLsizei w, GLsizei h, GLuint m, GLuint64 o), (t, l, f_, w, h, m, o))
FWD(glTexStorageMem3DEXT, "macvr_TexStorageMem3D", (GLenum t, GLsizei l, GLenum f_, GLsizei w, GLsizei h, GLsizei d, GLuint m, GLuint64 o), (t, l, f_, w, h, d, m, o))
FWD(glTextureViewOES, "macvr_TextureView", (GLuint v, GLenum t, GLuint o, GLenum f_, GLuint ml, GLuint nl, GLuint mly, GLuint nly), (v, t, o, f_, ml, nl, mly, nly))
FWD(glTextureViewEXT, "macvr_TextureView", (GLuint v, GLenum t, GLuint o, GLenum f_, GLuint ml, GLuint nl, GLuint mly, GLuint nly), (v, t, o, f_, ml, nl, mly, nly))
