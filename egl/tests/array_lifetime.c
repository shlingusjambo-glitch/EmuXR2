#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <sys/mman.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../../vk/fdmsg.h"
static int descriptors(void) {
    DIR *dir=opendir("/proc/self/fd"); assert(dir); int n=0;
    while(readdir(dir)) n++; closedir(dir); return n;
}
static int mappings(void) {
    FILE *f=fopen("/proc/self/maps","r"); assert(f); char line[1024]; int n=0;
    while(fgets(line,sizeof line,f)) if(strstr(line,"array-lifetime")) n++;
    fclose(f); return n;
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY); assert(eglInitialize(display,NULL,NULL));
    EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
    EGLConfig config; EGLint count; assert(eglChooseConfig(display,attrs,&config,1,&count)&&count);
    EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}, sa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca); assert(context!=EGL_NO_CONTEXT);
    EGLSurface surface=eglCreatePbufferSurface(display,config,sa); assert(eglMakeCurrent(display,surface,surface,context));
    void(*create)(GLsizei,GLuint*)=(void*)eglGetProcAddress("glCreateMemoryObjectsEXT");
    void(*import)(GLuint,GLuint64,GLenum,GLint)=(void*)eglGetProcAddress("glImportMemoryFdEXT");
    void(*storage)(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLsizei,GLuint,GLuint64)=(void*)eglGetProcAddress("glTexStorageMem3DEXT");
    void(*delmem)(GLsizei,const GLuint*)=(void*)eglGetProcAddress("glDeleteMemoryObjectsEXT");
    PFNEGLCREATESYNCKHRPROC sync=(void*)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC destroySync=(void*)eglGetProcAddress("eglDestroySyncKHR");
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC clientBuffer=(void*)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    PFNEGLCREATEIMAGEKHRPROC createImage=(void*)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroyImage=(void*)eglGetProcAddress("eglDestroyImageKHR");
    void(*bindImage)(GLenum,GLeglImageOES)=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    assert(create&&import&&storage&&delmem&&sync&&destroySync);
    int baseline=-1, baseMaps=-1, peak=0;
    for(int iteration=0;iteration<160;iteration++) {
        AHardwareBuffer *buffers[2];
        AHardwareBuffer_Desc desc={64,64,1,AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN};
        for(int i=0;i<2;i++) assert(!AHardwareBuffer_allocate(&desc,&buffers[i]));
        int gen=memfd_create("array-lifetime",MFD_CLOEXEC); assert(gen>=0); assert(!ftruncate(gen,4096));
        int fd=sendBuffers(buffers,2,2,1,gen); assert(fd>=0);
        GLuint memory,texture,fbo; create(1,&memory); import(memory,65536,0x9586,fd);
        glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D_ARRAY,texture);
        storage(GL_TEXTURE_2D_ARRAY,1,GL_RGBA8,64,64,2,memory,0);
        while(glGetError()!=GL_NO_ERROR) {} // ANGLE's unsupported external 3D allocation is replaced by ordinary storage.
        GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY,0,GL_TEXTURE_WIDTH,&width);assert(width==64);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,64,64);
        for(int layer=0;layer<2;layer++) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,texture,0,layer);
            assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
            glClearColor(layer==0,layer==1,0,1);glClear(GL_COLOR_BUFFER_BIT);
        }
        EGLSyncKHR fence=sync(display,EGL_SYNC_FENCE_KHR,NULL);assert(fence!=EGL_NO_SYNC_KHR);
        destroySync(display,fence);
        if(iteration%10==0) {
            glFinish();
            for(int layer=0;layer<2;layer++) {
                EGLImageKHR image=createImage(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,clientBuffer(buffers[layer]),NULL);
                assert(image!=EGL_NO_IMAGE_KHR);
                GLuint readTexture;glGenTextures(1,&readTexture);glBindTexture(GL_TEXTURE_2D,readTexture);
                bindImage(GL_TEXTURE_2D,image);destroyImage(display,image);
                glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,0,0,0);
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,readTexture,0);
                assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
                unsigned char pixels[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
                printf("eye %d rgba %u %u %u %u\n",layer,pixels[0],pixels[1],pixels[2],pixels[3]);
                assert(pixels[0]==(layer==0?255:0)&&pixels[1]==(layer==1?255:0)&&pixels[2]==0&&pixels[3]==255);
                glDeleteTextures(1,&readTexture);
            }
        }
        // Delete while completion callbacks may still own the mapping.
        glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);delmem(1,&memory);
        for(int layer=0;layer<2;layer++) AHardwareBuffer_release(buffers[layer]);close(gen);
        glFinish();usleep(10000);
        int now=descriptors(), maps=mappings(); if(now>peak) peak=now;
        if(iteration==10) {baseline=now;baseMaps=maps;}
        if(iteration>10) {assert(now<=baseline+4);assert(maps<=baseMaps+2);}
        if(iteration%20==0) printf("iteration %d: fd=%d maps=%d\n",iteration,now,maps);
    }
    printf("PASS: 160 stereo import/draw/delete cycles, correct eye pixels, bounded descriptors/mappings (peak fd %d)\n",peak);
    return 0;
}
