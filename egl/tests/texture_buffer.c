#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <stdio.h>
#include <stdlib.h>
void glTexBufferEXT(GLenum,GLenum,GLuint);
static GLuint shader(GLenum type,const char*s){GLuint sh=glCreateShader(type);glShaderSource(sh,1,&s,NULL);glCompileShader(sh);GLint ok;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);if(!ok){char log[4096];glGetShaderInfoLog(sh,sizeof log,NULL,log);puts(log);exit(1);}return sh;}
int main(){EGLDisplay d=eglGetDisplay(0);eglInitialize(d,0,0);EGLConfig c;EGLint n,ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};eglChooseConfig(d,ca,&c,1,&n);EGLint at[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},sa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};EGLContext x=eglCreateContext(d,c,0,at);EGLSurface surface=eglCreatePbufferSurface(d,c,sa);eglMakeCurrent(d,surface,surface,x);
const char*v="#version 310 es\n#extension GL_EXT_texture_buffer : require\nuniform highp samplerBuffer Values;\nout highp vec4 color;\nvoid main(){color=texelFetch(Values,0); gl_Position=vec4(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0,0,1);}";
const char*f="#version 310 es\n#extension GL_EXT_texture_buffer : require\nprecision highp float;in vec4 color;out vec4 outColor;void main(){outColor=color;}";
GLuint p=glCreateProgram();glAttachShader(p,shader(GL_VERTEX_SHADER,v));glAttachShader(p,shader(GL_FRAGMENT_SHADER,f));glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char l[4096];glGetProgramInfoLog(p,sizeof l,0,l);puts(l);return 1;}glUseProgram(p);
GLuint buffer,texture;glGenBuffers(1,&buffer);glBindBuffer(0x8c2a,buffer);float data[]={0.25,0.5,0.75,1.0};glBufferData(0x8c2a,sizeof data,data,GL_DYNAMIC_DRAW);glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0+3);glBindTexture(0x8c2a,texture);glTexBufferEXT(0x8c2a,GL_RGBA32F,buffer);glUniform1i(glGetUniformLocation(p,"Values"),3);
GLenum formats[]={GL_R32F,GL_RG32F,GL_RGB32F,GL_RGBA32F};
for(int components=1;components<=4;components++) {
    data[0]=.25;
    glBufferSubData(0x8c2a,0,sizeof data,data);
    glTexBufferEXT(0x8c2a,formats[components-1],buffer);
    for(int pass=0;pass<2;pass++) {
        if(pass){data[0]=.75;glBufferSubData(0x8c2a,0,sizeof data,data);}
        glViewport(0,0,1,1);glDrawArrays(GL_TRIANGLES,0,3);
        unsigned char pixel[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        GLenum error=glGetError();
        printf("%d components, pass %d: %u %u %u %u error=%x\n",components,pass,pixel[0],pixel[1],pixel[2],pixel[3],error);
        if(error || abs(pixel[0]-(pass?191:64))>2 || abs(pixel[1]-(components>1?128:0))>2 || abs(pixel[2]-(components>2?191:0))>2 || pixel[3]!=255)return 2;
    }
}
puts("PASS R32F/RG32F/RGB32F/RGBA32F texture buffer fetch and live update");return 0;}
