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

// Qualcomm's compiler (which Meta's shaders were written against) accepts #extension after other code and a fragment
// shader with no default float precision; ANGLE follows the spec. Top-level #extension lines move up under #version,
// and fragment shaders get a default float precision (a later declaration still overrides it).
static char *relaxed(const char *s, int frag) {
    size_t n = strlen(s);
    char *ext = calloc(n + 1, 1), *body = calloc(n + 64, 1), *out = malloc(2 * n + 64);
    const char *ver = NULL, *verEnd = NULL; int depth = 0, precise = !frag;
    for (const char *l = s; *l; ) {
        const char *e = strchr(l, '\n'); e = e ? e + 1 : l + strlen(l);
        const char *q = l; while (*q == ' ' || *q == '\t' || *q == '\r') q++;
        int top = depth == 0;
        if (!strncmp(q, "#if", 3)) depth++;
        else if (!strncmp(q, "#endif", 6)) depth--;
        if (!ver && !strncmp(q, "#version", 8)) { ver = l; verEnd = e; }
        else if (top && !strncmp(q, "#extension", 10)) { strncat(ext, l, e - l); if (e[-1] != '\n') strcat(ext, "\n"); }
        else {
            // the default precision goes right before the first top-level code (after any #extension in #if blocks)
            if (!precise && top && *q != '#' && *q != '\n' && *q && strncmp(q, "//", 2)) { strcat(body, "precision highp float;\n"); precise = 1; }
            strncat(body, l, e - l);
        }
        l = e;
    }
    out[0] = 0;
    if (ver) { strncat(out, ver, verEnd - ver); if (verEnd[-1] != '\n') strcat(out, "\n"); }
    strcat(out, ext); strcat(out, body);
    free(ext); free(body);
    return out;
}

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
    GLint type = 0; glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    char *t = relaxed(s, type == GL_FRAGMENT_SHADER);
    const GLchar *one = t;
    f(shader, 1, &one, NULL);
    free(t); free(s);
}

// GL_OES_texture_view is emulated (in egl_macvr.c) for Meta VR clients, which use it to wrap shared swapchains
static int wantsViews(void) {
    static int v = -1;
    if (v < 0) { char n[64] = {0}; FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { fread(n, 1, 63, f); fclose(f); } v = !strncmp(n, "com.oculus.", 11); }
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

// Keep real failures visible while diagnosing Quest-specific swapchain attachments.
GLenum glCheckFramebufferStatus(GLenum target) {
    static GLenum (*f)(GLenum);
    if (!f) f = dlsym(RTLD_NEXT, "glCheckFramebufferStatus");
    GLenum status = f(target);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        static void (*get)(GLenum, GLenum, GLenum, GLint *);
        if (!get) get = dlsym(RTLD_NEXT, "glGetFramebufferAttachmentParameteriv");
        const GLenum attachments[] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
        for (unsigned i = 0; i < 3; ++i) {
            GLint type = 0, id = 0;
            get(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            get(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &id);
            LOG("framebuffer 0x%x status 0x%x: attachment 0x%x type 0x%x id %d", target, status, attachments[i], type, id);
            if (type == GL_TEXTURE) {
                GLint level = 0, layer = 0, views = 0, base = 0, fmt = 0, w = 0, h = 0, d = 0, immut = 0, cur = 0;
                get(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
                get(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
                get(target, attachments[i], 0x9630 /* NUM_VIEWS_OVR */, &views);
                get(target, attachments[i], 0x9632 /* BASE_VIEW_INDEX_OVR */, &base);
                glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &cur);
                glBindTexture(GL_TEXTURE_2D_ARRAY, id);
                glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_WIDTH, &w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_HEIGHT, &h);
                glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_DEPTH, &d);
                glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_IMMUTABLE_FORMAT, &immut);
                glBindTexture(GL_TEXTURE_2D_ARRAY, cur);
                LOG("  level %d layer %d views %d+%d; as 2D array: format 0x%x %dx%dx%d immutable %d error 0x%x", level, layer, base, views, fmt, w, h, d, immut, glGetError());
            }
        }
    }
    return status;
}

// framebuffer attachments: arrays emulated in libEGL_macvr need copying out once rendered (and failures are logged)
static void noteAttach(GLuint tex) { static void (*f)(GLuint); if (!f) f = eglShim("macvr_NoteAttach"); if (f) f(tex); }
#define ATTACH(name, params, args, tex, fmt, ...) void name params { static void (*f) params; static GLenum (*err)(void); \
    if (!f) { f = dlsym(RTLD_NEXT, #name); err = dlsym(RTLD_NEXT, "glGetError"); } \
    noteAttach(tex); if (!f) { LOG(#name ": not in the driver"); return; } f args; GLenum e = err(); if (e) LOG(#name fmt ": GL error 0x%x", __VA_ARGS__, e); }
ATTACH(glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l), (t, a, tt, tex, l), tex, "(0x%x, 0x%x, tex %u)", a, tt, tex)
ATTACH(glFramebufferTextureLayer, (GLenum t, GLenum a, GLuint tex, GLint l, GLint layer), (t, a, tex, l, layer), tex, "(0x%x, tex %u, layer %d)", a, tex, layer)
ATTACH(glFramebufferTextureMultiviewOVR, (GLenum t, GLenum a, GLuint tex, GLint l, GLint base, GLsizei n), (t, a, tex, l, base, n), tex, "(0x%x, tex %u, views %d+%d)", a, tex, base, n)
ATTACH(glFramebufferTextureMultisampleMultiviewOVR, (GLenum t, GLenum a, GLuint tex, GLint l, GLsizei s, GLint base, GLsizei n), (t, a, tex, l, s, base, n), tex, "(0x%x, tex %u, views %d+%d)", a, tex, base, n)
GLsync glFenceSync(GLenum c, GLbitfield fl) {
    static GLsync (*f)(GLenum, GLbitfield); static void (*flush)(void);
    if (!f) { f = dlsym(RTLD_NEXT, "glFenceSync"); flush = eglShim("macvr_FlushArrays"); }
    if (flush) flush();
    return f(c, fl);
}
// shader compile failures: ANGLE's log and the source's first lines
void glCompileShader(GLuint sh) {
    static void (*f)(GLuint); if (!f) f = dlsym(RTLD_NEXT, "glCompileShader");
    f(sh);
    GLint ok = 1; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) return;
    char log[1024] = {0}, src[1536] = {0};
    glGetShaderInfoLog(sh, sizeof log, NULL, log); glGetShaderSource(sh, sizeof src, NULL, src);
    LOG("shader %u failed: %s", sh, log);
    for (char *l = strtok(src, "\n"); l; l = strtok(NULL, "\n")) LOG("  | %s", l);
}
