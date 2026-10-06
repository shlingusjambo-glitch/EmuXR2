// Float buffer textures for ANGLE/Vulkan ES 3.1. Shader storage buffers retain
// the application's backing buffer, including subsequent glBufferSubData writes.
// This path supports R32F, RG32F, RGB32F and RGBA32F; integer formats are rejected.
#include <ctype.h>
#define MACVR_TEXTURE_BUFFER 0x8C2A
#define MACVR_MAX_BUFFER_SAMPLERS 8
static char *macvrReplace(char *s, const char *from, const char *to) {
    size_t a = strlen(from), b = strlen(to), count = 0;
    for (char *p = s; (p = strstr(p, from)); p += a) count++;
    char *out = malloc(strlen(s) + count * (b > a ? b - a : 0) + 1), *q = out;
    const char *p = s, *next;
    while ((next = strstr(p, from))) { size_t n = next - p; memcpy(q,p,n); q+=n; memcpy(q,to,b); q+=b; p=next+a; }
    strcpy(q,p); free(s); return out;
}
static char *macvrBufferShader(char *s) {
    s=macvrReplace(s,"#extension GL_EXT_texture_buffer : require","// buffer textures translated to shader storage buffers");
    if (!strstr(s,"samplerBuffer")) return s;
    // Conditional declarations also get translated, remaining in their #if branches.
    const char *types[] = {"uniform highp samplerBuffer ","uniform samplerBuffer "};
    unsigned slot = 0;
    for (unsigned type = 0; type < 2; type++) {
        char *p;
        while ((p = strstr(s,types[type])) && slot < MACVR_MAX_BUFFER_SAMPLERS) {
            char name[128]; const char *start = p + strlen(types[type]); unsigned n=0;
            while ((isalnum((unsigned char)start[n]) || start[n]=='_') && n<sizeof name-1) {name[n]=start[n];n++;} name[n]=0;
            if (!n || start[n]!=';') { LOG("unsupported buffer sampler declaration"); return s; }
            char declaration[256], replacement[2048];
            snprintf(declaration,sizeof declaration,"%s%s;",types[type],name);
            snprintf(replacement,sizeof replacement,
                "uniform highp int %s;\nuniform highp int macvr_Stride_%s;\n"
                "layout(std430, binding=%u) readonly buffer macvr_Block_%s { highp float macvr_Data_%s[]; };\n"
                "highp vec4 macvr_Fetch_%s(highp int index) {\n"
                "if (%s < 0 || index < 0) return vec4(0.0);\n"
                "highp int stride=macvr_Stride_%s; highp int base=index*stride;\n"
                "return vec4(macvr_Data_%s[base], stride>1 ? macvr_Data_%s[base+1] : 0.0,"
                "stride>2 ? macvr_Data_%s[base+2] : 0.0, stride>3 ? macvr_Data_%s[base+3] : 1.0); }",
                name,name,slot++,name,name,name,name,name,name,name,name,name);
            s=macvrReplace(s,declaration,replacement);
            char fetch[256], function[256];
            snprintf(fetch,sizeof fetch,"texelFetch(%s,",name);
            snprintf(function,sizeof function,"macvr_Fetch_%s(",name);
            s=macvrReplace(s,fetch,function);
        }
    }
    s=macvrReplace(s,"#extension GL_EXT_texture_buffer : require","// buffer textures translated to shader storage buffers");
    return s;
}
// State belongs to an EGL context, not a thread: VrShell switches contexts.
#include <EGL/egl.h>
static void *macvrCurrentContext(void) { static void *(*f)(void); if(!f) {void*h=dlopen("libEGL_angle.so",RTLD_NOW|RTLD_NOLOAD);f=h?dlsym(h,"eglGetCurrentContext"):NULL;} return f?f():NULL; }
struct MacvrBufferTexture {GLuint texture,buffer; GLint stride;};
struct MacvrBufferSampler {GLint location,strideLocation,binding;};
struct MacvrBufferProgram {GLuint program; unsigned count; struct MacvrBufferSampler samplers[8];};
struct MacvrBufferContext {void*context; GLuint active,units[32]; struct MacvrBufferTexture textures[256]; struct MacvrBufferProgram programs[128],*current;};
static __thread struct MacvrBufferContext macvrContexts[8];
static struct MacvrBufferContext *macvrBufferContext(void) {
    void *context=macvrCurrentContext();
    for(unsigned i=0;i<8;i++) if(macvrContexts[i].context==context || !macvrContexts[i].context) {macvrContexts[i].context=context;return &macvrContexts[i];}
    return NULL;
}
static struct MacvrBufferTexture *macvrBufferTexture(struct MacvrBufferContext*c,GLuint name) {
    if(!c || !name)return NULL;
    for(unsigned i=0;i<256;i++)if(c->textures[i].texture==name || !c->textures[i].texture){c->textures[i].texture=name;return &c->textures[i];}
    return NULL;
}
static void macvrUseBufferProgram(GLuint program) {
    struct MacvrBufferContext*c=macvrBufferContext();if(!c)return;c->current=NULL;if(!program)return;
    struct MacvrBufferProgram*p=NULL;
    for(unsigned i=0;i<128;i++)if(c->programs[i].program==program){c->current=&c->programs[i];return;}else if(!p&&!c->programs[i].program)p=&c->programs[i];
    if(!p)return;p->program=program;p->count=0;c->current=p;
    GLint count=0;glGetProgramiv(program,GL_ACTIVE_UNIFORMS,&count);
    for(GLint i=0;i<count&&p->count<8;i++){
        char name[256];GLint size;GLenum type;glGetActiveUniform(program,i,sizeof name,NULL,&size,&type,name);
        if(strncmp(name,"macvr_Stride_",13))continue;
        const char*sampler=name+13;GLint location=glGetUniformLocation(program,sampler);if(location<0)continue;
        char block[256];snprintf(block,sizeof block,"macvr_Block_%s",sampler);
        GLuint index=glGetProgramResourceIndex(program,GL_SHADER_STORAGE_BLOCK,block);if(index==GL_INVALID_INDEX)continue;
        GLenum property=GL_BUFFER_BINDING;GLint binding=0;glGetProgramResourceiv(program,GL_SHADER_STORAGE_BLOCK,index,1,&property,1,NULL,&binding);
        p->samplers[p->count++]=(struct MacvrBufferSampler){location,glGetUniformLocation(program,name),binding};
    }
}
static void macvrForgetBufferProgram(GLuint program) {
    struct MacvrBufferContext*c=macvrBufferContext();if(!c)return;
    for(unsigned i=0;i<128;i++)if(c->programs[i].program==program){if(c->current==&c->programs[i])c->current=NULL;memset(&c->programs[i],0,sizeof c->programs[i]);}
}
static void macvrBindBufferSamplers(void) {
    struct MacvrBufferContext*c=macvrBufferContext();if(!c||!c->current||!c->current->count)return;
    struct MacvrBufferProgram*p=c->current;
    static void(*set)(GLint,GLint);if(!set)set=dlsym(RTLD_NEXT,"glUniform1i");
    for(unsigned i=0;i<p->count;i++){
        struct MacvrBufferSampler*sampler=&p->samplers[i];GLint unit=0;
        glGetUniformiv(p->program,sampler->location,&unit);if(unit<0||unit>=32)continue;
        struct MacvrBufferTexture*t=macvrBufferTexture(c,c->units[unit]);if(!t)continue;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,sampler->binding,t->buffer);
        set(sampler->strideLocation,t->stride);
    }
}
void glDeleteProgram(GLuint program){static void(*f)(GLuint);if(!f)f=dlsym(RTLD_NEXT,"glDeleteProgram");macvrForgetBufferProgram(program);f(program);}
void glActiveTexture(GLenum unit) {static void(*f)(GLenum);if(!f)f=dlsym(RTLD_NEXT,"glActiveTexture");f(unit);struct MacvrBufferContext*c=macvrBufferContext();if(c)c->active=unit-GL_TEXTURE0;}
void glBindTexture(GLenum target,GLuint texture) {
    static void(*f)(GLenum,GLuint);if(!f)f=dlsym(RTLD_NEXT,"glBindTexture");
    if(target!=MACVR_TEXTURE_BUFFER){f(target,texture);return;}
    struct MacvrBufferContext*c=macvrBufferContext();if(c&&c->active<32)c->units[c->active]=texture;
    macvrBufferTexture(c,texture);macvrBindBufferSamplers();
}
void glTexBufferEXT(GLenum target,GLenum format,GLuint buffer) {
    if(target!=MACVR_TEXTURE_BUFFER){LOG("unsupported texture buffer target 0x%x",target);return;}
    GLint stride=format==GL_R32F?1:format==GL_RG32F?2:format==GL_RGB32F?3:format==GL_RGBA32F?4:0;
    if(!stride){LOG("unsupported texture buffer format 0x%x",format);return;}
    struct MacvrBufferContext*c=macvrBufferContext();struct MacvrBufferTexture*t=c&&c->active<32?macvrBufferTexture(c,c->units[c->active]):NULL;
    if(t){t->buffer=buffer;t->stride=stride;macvrBindBufferSamplers();}
}
void glTexBuffer(GLenum target,GLenum format,GLuint buffer){glTexBufferEXT(target,format,buffer);}
void glBindBuffer(GLenum target,GLuint buffer){static void(*f)(GLenum,GLuint);if(!f)f=dlsym(RTLD_NEXT,"glBindBuffer");f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target,buffer);}
void glBufferData(GLenum target,GLsizeiptr size,const void*data,GLenum usage){static void(*f)(GLenum,GLsizeiptr,const void*,GLenum);if(!f)f=dlsym(RTLD_NEXT,"glBufferData");f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target,size,data,usage);}
void glBufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void*data){static void(*f)(GLenum,GLintptr,GLsizeiptr,const void*);if(!f)f=dlsym(RTLD_NEXT,"glBufferSubData");f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target,offset,size,data);}

void *glMapBufferRange(GLenum target,GLintptr offset,GLsizeiptr length,GLbitfield access){static void*(*f)(GLenum,GLintptr,GLsizeiptr,GLbitfield);if(!f)f=dlsym(RTLD_NEXT,"glMapBufferRange");return f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target,offset,length,access);}
GLboolean glUnmapBuffer(GLenum target){static GLboolean(*f)(GLenum);if(!f)f=dlsym(RTLD_NEXT,"glUnmapBuffer");return f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target);}
void glGetBufferParameteriv(GLenum target,GLenum pname,GLint*params){static void(*f)(GLenum,GLenum,GLint*);if(!f)f=dlsym(RTLD_NEXT,"glGetBufferParameteriv");f(target==MACVR_TEXTURE_BUFFER?GL_COPY_WRITE_BUFFER:target,pname,params);}
