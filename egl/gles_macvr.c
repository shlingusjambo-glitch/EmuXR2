// MacVR GLES v2/v3 shim over ANGLE (a DT_NEEDED dependency, so every other gl* entry point resolves there).
// ANGLE on the emulator's Vulkan tops out at ES 3.1; Meta's shaders declare 320 es but (so far) use 3.1
// features, so the version line is rewritten.
#include <GLES3/gl31.h>
#include <android/log.h>
#include <dlfcn.h>
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
