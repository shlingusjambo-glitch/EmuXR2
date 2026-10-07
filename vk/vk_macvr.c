// MacVR Vulkan HAL wrapper for the emulator's vendor layer (ro.hardware.vulkan=macvr).
//
// Forwards everything to the emulator's own driver (vulkan.ranchu.so) and adds VK_KHR_external_memory_fd, which
// the Quest's VR runtime uses to hand swapchain images to client apps and which the emulator's driver lacks.
//
// The only memory the emulator can share between processes is a 2D, single-layer, single-mip AHardwareBuffer.
//  - Plain 2D external images are backed by one directly (zero copy).
//  - Layered (stereo multiview) or mipmapped external images stay ordinary images in each process, with one
//    shared 2D "shadow" per layer and mip. A process's writes are copied into the shadows right after the submit
//    that made them, and a shared generation counter is bumped once that copy has finished on the GPU; any other
//    process holding the image copies the shadows back in at its next submit after the counter moves.
// The "opaque fd" is one end of a unix socket holding a single message: a header, each shadow's flattened
// AHardwareBuffer, and the counter's memfd. Importers peek at it, so one fd can be imported any number of times.
#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MacVR-VK", __VA_ARGS__)
#define OPAQUE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define AHB VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
#define MAXSH 32      // shadows per image (layers x mips)
#define MAXSHARED 512

static hwvulkan_device_t *realDev;
static PFN_vkGetInstanceProcAddr realGIPA;
static PFN_vkGetDeviceProcAddr realGDPA;
static VkInstance gInst;   // the most recent instance: instance-level lookups need one
#define IPA(n) realGIPA(gInst, n)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// ---- per-device state ----
#define FN(x) PFN_vk##x x
typedef struct {
    VkDevice dev; VkPhysicalDevice pd; VkPhysicalDeviceMemoryProperties mp;
    FN(CreateImage); FN(DestroyImage); FN(CreateBuffer); FN(AllocateMemory); FN(FreeMemory); FN(BindImageMemory); FN(BindImageMemory2);
    FN(GetImageMemoryRequirements); FN(GetImageMemoryRequirements2);
    FN(GetMemoryAndroidHardwareBufferANDROID); FN(GetAndroidHardwareBufferPropertiesANDROID);
    FN(CreateCommandPool); FN(AllocateCommandBuffers); FN(FreeCommandBuffers); FN(BeginCommandBuffer); FN(EndCommandBuffer);
    FN(CmdPipelineBarrier); FN(CmdCopyImage); FN(QueueSubmit); FN(QueueSubmit2); FN(QueueSubmit2KHR); FN(GetDeviceQueue); FN(GetDeviceQueue2);
    FN(CreateFence); FN(DestroyFence); FN(WaitForFences); FN(GetFenceStatus); FN(WaitSemaphores); FN(WaitSemaphoresKHR); FN(GetSemaphoreCounterValue); FN(GetSemaphoreCounterValueKHR);
    FN(CreateImageView); FN(DestroyImageView); FN(CreateFramebuffer); FN(DestroyFramebuffer);
    FN(CmdBeginRenderPass); FN(CmdBeginRenderPass2); FN(CmdBeginRenderPass2KHR);
    FN(CmdClearColorImage); FN(CmdBeginRendering); FN(CmdBeginRenderingKHR); FN(CmdPipelineBarrier2); FN(CmdPipelineBarrier2KHR); FN(CmdExecuteCommands); FN(CmdDraw); FN(CmdDrawIndexed); FN(CmdCopyImage2); FN(CmdBlitImage2); FN(CmdResolveImage2); FN(CmdCopyBufferToImage2); FN(CmdCopyImage2KHR); FN(CmdBlitImage2KHR); FN(CmdResolveImage2KHR); FN(CmdCopyBufferToImage2KHR); FN(CreateRenderPass); FN(CreateRenderPass2); FN(CmdNextSubpass); FN(CmdEndRenderPass); FN(CmdBlitImage); FN(CmdCopyBufferToImage); FN(CmdResolveImage);
    VkCommandPool pool[8];   // per queue family, for our copy command buffers
    VkImage lastExt;         // external image whose requirements were queried last
} Dev;
static Dev devs[16];
static Dev *findDev(VkDevice d) {
    for (int i = 0; i < 16; i++) if (devs[i].dev == d) return &devs[i];
    return NULL;
}
static uint32_t pickType(Dev *d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return bits ? (uint32_t)__builtin_ctz(bits) : 0;
}

// ---- shared images ----
typedef struct Shared {
    VkDevice dev; Dev *d;
    VkImage image; VkImageCreateInfo ci;   // the app-visible image (ci.pNext is dropped)
    int complex, exporter, ready;
    VkDeviceMemory mem;                     // the app-visible image's memory
    uint32_t n; VkImage sh[MAXSH]; VkDeviceMemory shm[MAXSH];
    volatile atomic_uint *gen; int genFd;   // shared generation counter (complex images)
    unsigned seen;                          // generation this process last has (copied in, or wrote itself)
    VkCommandBuffer in[8], out[8];          // per queue family: shadows->image, image->shadows
} Shared;
static Shared *shared[MAXSHARED];
static Shared *byImage(VkImage im) {
    if (!im) return NULL;
    for (int i = 0; i < MAXSHARED; i++) if (shared[i] && shared[i]->image == im) return shared[i];
    return NULL;
}
static Shared *byMem(VkDeviceMemory m) {
    if (!m) return NULL;
    for (int i = 0; i < MAXSHARED; i++) if (shared[i] && shared[i]->mem == m) return shared[i];
    return NULL;
}
static void addShared(Shared *s) {
    for (int i = 0; i < MAXSHARED; i++) if (!shared[i]) { shared[i] = s; return; }
    LOG("too many shared images");
}

static const VkBaseOutStructure *find(const void *chain, VkStructureType t) {
    for (const VkBaseOutStructure *s = chain; s; s = s->pNext) if (s->sType == t) return s;
    return NULL;
}
// unlink one struct from a (const) pNext chain for the duration of a call; returns the link to restore
static VkBaseOutStructure *unlink_(const void *head, const void *node) {
    VkBaseOutStructure *l = (VkBaseOutStructure *)head;
    while (l->pNext && l->pNext != node) l = l->pNext;
    if (l->pNext) { l->pNext = ((VkBaseOutStructure *)node)->pNext; return l; }
    return NULL;
}
static void relink(VkBaseOutStructure *l, const void *node) { if (l) l->pNext = (void *)node; }

static VkFormat ahbFormat(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

static VkImageAspectFlags formatAspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

#include "fdmsg.h"

// ---- shadows and copies ----
static VkResult makeShadow(Shared *s, uint32_t i, AHardwareBuffer *import, AHardwareBuffer **exported) {
    Dev *d = s->d; uint32_t mip = i % s->ci.mipLevels;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, AHB};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, ahbFormat(s->ci.format),
        {s->ci.extent.width >> mip ? s->ci.extent.width >> mip : 1, s->ci.extent.height >> mip ? s->ci.extent.height >> mip : 1, 1},
        1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            (formatAspect(s->ci.format) == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = d->CreateImage(s->dev, &ci, NULL, &s->sh[i]);
    if (r) return r;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, s->sh[i], VK_NULL_HANDLE};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, AHB};
    VkImportAndroidHardwareBufferInfoANDROID im = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, import};
    if (import) {
        VkAndroidHardwareBufferPropertiesANDROID p = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        d->GetAndroidHardwareBufferPropertiesANDROID(s->dev, import, &p);
        ded.pNext = &im; ai.allocationSize = p.allocationSize; ai.memoryTypeIndex = pickType(d, p.memoryTypeBits, 0);
    } else {
        VkMemoryRequirements mr; d->GetImageMemoryRequirements(s->dev, s->sh[i], &mr);
        ded.pNext = &ex; ai.allocationSize = mr.size; ai.memoryTypeIndex = pickType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if ((r = d->AllocateMemory(s->dev, &ai, NULL, &s->shm[i]))) {
        // A failed driver allocation does not own a memory object, even if the
        // guest transport wrote a nonzero handle into its output slot.
        s->shm[i] = VK_NULL_HANDLE;
        return r;
    }
    if ((r = d->BindImageMemory(s->dev, s->sh[i], s->shm[i], 0))) return r;
    if (exported) {
        VkMemoryGetAndroidHardwareBufferInfoANDROID gi = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, s->shm[i]};
        r = d->GetMemoryAndroidHardwareBufferANDROID(s->dev, &gi, exported);
    }
    return r;
}

static VkCommandBuffer copyCmd(Shared *s, uint32_t family, int toImage) {
    Dev *d = s->d;
    if (family >= 8) return VK_NULL_HANDLE;
    VkCommandBuffer *slotp = toImage ? &s->in[family] : &s->out[family];
    if (*slotp) return *slotp;
    if (!d->pool[family]) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (d->CreateCommandPool(s->dev, &pi, NULL, &d->pool[family])) return VK_NULL_HANDLE;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, d->pool[family], VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    if (d->AllocateCommandBuffers(s->dev, &ai, &cb)) return VK_NULL_HANDLE;
    *(void **)cb = *(void **)s->dev;   // dispatchable object: share the device's dispatch pointer
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    d->BeginCommandBuffer(cb, &bi);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (uint32_t i = 0; i < s->n; i++) {
        uint32_t layer = i / s->ci.mipLevels, mip = i % s->ci.mipLevels;
        VkExtent3D e = {s->ci.extent.width >> mip ? s->ci.extent.width >> mip : 1, s->ci.extent.height >> mip ? s->ci.extent.height >> mip : 1, 1};
        for (VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT; asp <= VK_IMAGE_ASPECT_STENCIL_BIT; asp <<= 1) {
            if (!(formatAspect(s->ci.format) & asp)) continue;
            VkImageCopy c = {{asp, mip, layer, 1}, {0, 0, 0}, {asp, 0, 0, 1}, {0, 0, 0}, e};
            if (toImage) {   // shadows -> image
                VkImageCopy b = {{asp, 0, 0, 1}, {0, 0, 0}, {asp, mip, layer, 1}, {0, 0, 0}, e};
                d->CmdCopyImage(cb, s->sh[i], VK_IMAGE_LAYOUT_GENERAL, s->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b);
            } else d->CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, s->sh[i], VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, NULL, 0, NULL);
    d->EndCommandBuffer(cb);
    return *slotp = cb;
}

static void freeShared(Shared *s) {
    Dev *d = s->d;
    for (int f = 0; f < 8; f++) {
        if (s->in[f]) d->FreeCommandBuffers(s->dev, d->pool[f], 1, &s->in[f]);
        if (s->out[f]) d->FreeCommandBuffers(s->dev, d->pool[f], 1, &s->out[f]);
    }
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->sh[i]) d->DestroyImage(s->dev, s->sh[i], NULL);
        if (s->shm[i]) d->FreeMemory(s->dev, s->shm[i], NULL);
    }
    if (s->gen) munmap((void *)s->gen, 4096);
    if (s->genFd >= 0) close(s->genFd);
    for (int i = 0; i < MAXSHARED; i++) if (shared[i] == s) shared[i] = NULL;
    free(s);
}

// imports of layered/mipmapped memory that arrive before their image (ANGLE imports, then creates the image):
// the shadows are made when the image is bound
typedef struct { VkDevice dev; VkDeviceMemory mem; Recv rv; } Pending;
static Pending pending[64];

// ---- command buffer write tracking (importers: which shared images a submit writes) ----
#define MAXT 2048
typedef struct { void *key; Shared *s[8]; } Track;
static Track views[256], fbs[256], cbs[MAXT];
static Track *slot(Track *t, int cap, void *key, int create) {
    unsigned h = ((uintptr_t)key >> 4) % cap;
    for (int i = 0; i < cap; i++, h = (h + 1) % cap) {
        if (t[h].key == key) return &t[h];
        if (!t[h].key) { if (!create) return NULL; t[h].key = key; memset(t[h].s, 0, sizeof t[h].s); return &t[h]; }
    }
    return NULL;
}
static void addTo(Track *t, Shared *s) {
    if (!t || !s) return;
    for (int i = 0; i < 8; i++) { if (t->s[i] == s) return; if (!t->s[i]) { t->s[i] = s; return; } }
}
static void markCb(VkCommandBuffer cb, Shared *s) {
    if (!s || !s->complex) return;
    if (s->ci.arrayLayers > 1) { static unsigned n; if (n++ % 500 == 0) LOG("stereo marked #%u %ux%u", n, s->ci.extent.width, s->ci.extent.height); }
    pthread_mutex_lock(&lock); addTo(slot(cbs, MAXT, cb, 1), s); pthread_mutex_unlock(&lock);
}

// ---- completion worker: bump generations once the app's copies are done ----
typedef struct Job { Dev *d; VkFence fence; Shared *s[64]; int n; struct Job *next; } Job;
static Job *jobs; static pthread_cond_t jobCv = PTHREAD_COND_INITIALIZER; static pthread_t worker;
static void *workerMain(void *u) {
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!jobs) pthread_cond_wait(&jobCv, &lock);
        Job *j = jobs; jobs = j->next;
        pthread_mutex_unlock(&lock);
        j->d->WaitForFences(j->d->dev, 1, &j->fence, VK_TRUE, UINT64_MAX);
        pthread_mutex_lock(&lock);
        for (int i = 0; i < j->n; i++) if (j->s[i]->gen) j->s[i]->seen = atomic_fetch_add(j->s[i]->gen, 1) + 1;
        pthread_mutex_unlock(&lock);
        j->d->DestroyFence(j->d->dev, j->fence, NULL);
        free(j);
    }
    return u;
}

// extensions the wrapper provides itself (removed before the driver sees them)
// (VK_KHR_multiview is core in 1.1; the guest driver supports it but does not list the name, which ANGLE checks for)
// VK_EXT_line_rasterization is only claimed (Bresenham lines) because ANGLE exposes GL_OVR_multiview only with it;
// lines are rasterized however the host driver draws them by default.
// VK_KHR_depth_stencil_resolve is required by newer Horizon home environments (v64's shellenv won't create a device
// without it); render passes 2 become v1 here, which has no depth resolve, so a resolve attachment keeps its contents.
// Claimed only to Meta's own apps (com.oculus.*), so games keep the extension list they always had.
// ponytail: depth resolve not performed; add a sample-0 copy at the end of the subpass if something reads it back
static int metaApp(void) {
    static int v = -1;
    if (v < 0) { char n[64] = {0}; FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { fread(n, 1, sizeof n - 1, f); fclose(f); } v = !strncmp(n, "com.oculus.", 11); }
    return v;
}
static const char *emulated[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME,
    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME};

// ---- instance level ----
static VkResult EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *props) {
    PFN_vkEnumerateDeviceExtensionProperties f = (void *)IPA("vkEnumerateDeviceExtensionProperties");
    if (layer) return f(pd, layer, count, props);
    uint32_t n = 0; f(pd, NULL, &n, NULL);
    VkExtensionProperties *all = calloc(n + 1, sizeof *all);
    f(pd, NULL, &n, all);
    all = realloc(all, (n + 8) * sizeof *all);
    for (unsigned k = 0; k < sizeof emulated / sizeof *emulated; k++) {
        int have = 0;
        for (uint32_t i = 0; i < n; i++) if (!strcmp(all[i].extensionName, emulated[k])) have = 1;
        if (!strcmp(emulated[k], VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) && !metaApp()) have = 1;
        if (!have) { memset(&all[n], 0, sizeof *all); strcpy(all[n].extensionName, emulated[k]); all[n].specVersion = 1; n++; }
    }
    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else { if (*count < n) r = VK_INCOMPLETE; else *count = n; memcpy(props, all, *count * sizeof *all); }
    free(all); return r;
}

static VkResult CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    PFN_vkCreateDevice f = (void *)IPA("vkCreateDevice");
    if (!realGDPA) realGDPA = (PFN_vkGetDeviceProcAddr)IPA("vkGetDeviceProcAddr");
    PFN_vkEnumerateDeviceExtensionProperties en = (void *)IPA("vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0; en(pd, NULL, &n, NULL);
    VkExtensionProperties *avail = calloc(n, sizeof *avail); en(pd, NULL, &n, avail);
    // drop our extension, add the ones that back it
    static const char *need[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME};
    const char **ext = calloc(ci->enabledExtensionCount + 8, sizeof *ext);
    uint32_t m = 0; int wantFd = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i]; int ours = 0;
        if (!strcmp(e, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) wantFd = 1;
        for (unsigned k = 0; k < sizeof emulated / sizeof *emulated; k++) if (!strcmp(e, emulated[k])) {
            int real = 0;   // keep it if the driver has it after all
            for (uint32_t j = 0; j < n; j++) if (!strcmp(avail[j].extensionName, e)) real = 1;
            ours = !real;
        }
        if (!ours) ext[m++] = e;
    }
    // always: the runtime's client library imports swapchain memory on app devices that didn't ask for it
    for (unsigned k = 0; k < sizeof need / sizeof *need; k++) {
            int on = 0, ok = 0;
            for (uint32_t i = 0; i < m; i++) if (!strcmp(ext[i], need[k])) on = 1;
            for (uint32_t i = 0; i < n; i++) if (!strcmp(avail[i].extensionName, need[k])) ok = 1;
            if (!on && ok) ext[m++] = need[k];
        }
    VkDeviceCreateInfo c = *ci; c.enabledExtensionCount = m; c.ppEnabledExtensionNames = ext;
    // the line rasterization features struct is ours: unlink it for the call
    VkBaseOutStructure *prev = (VkBaseOutStructure *)&c, *lineFeat = NULL;
    for (; prev->pNext; prev = prev->pNext)
        if (prev->pNext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT) { lineFeat = prev->pNext; prev->pNext = lineFeat->pNext; break; }
    for (uint32_t i = 0; i < m; i++) {
        int ok = 0; for (uint32_t j = 0; j < n; j++) if (!strcmp(avail[j].extensionName, ext[i])) ok = 1;
        if (!ok) LOG("vkCreateDevice: driver lacks %s", ext[i]);
    }
    VkResult r = f(pd, &c, a, out);
    if (lineFeat) prev->pNext = lineFeat;
    free(ext); free(avail);
    if (r != VK_SUCCESS) { LOG("vkCreateDevice failed %d", r); return r; }
    pthread_mutex_lock(&lock);
    Dev *d = findDev(NULL);
    if (d) {
        memset(d, 0, sizeof *d);
        d->dev = *out; d->pd = pd;
        ((PFN_vkGetPhysicalDeviceMemoryProperties)IPA("vkGetPhysicalDeviceMemoryProperties"))(pd, &d->mp);
#define GET(x) d->x = (void *)realGDPA(*out, "vk" #x)
        GET(CreateImage); GET(DestroyImage); GET(CreateBuffer); GET(AllocateMemory); GET(FreeMemory); GET(BindImageMemory); GET(BindImageMemory2);
        if (!d->BindImageMemory2) d->BindImageMemory2 = (void *)realGDPA(*out, "vkBindImageMemory2KHR");
        GET(GetImageMemoryRequirements); GET(GetImageMemoryRequirements2);
        if (!d->GetImageMemoryRequirements2) d->GetImageMemoryRequirements2 = (void *)realGDPA(*out, "vkGetImageMemoryRequirements2KHR");
        GET(GetMemoryAndroidHardwareBufferANDROID); GET(GetAndroidHardwareBufferPropertiesANDROID);
        GET(CreateCommandPool); GET(AllocateCommandBuffers); GET(FreeCommandBuffers); GET(BeginCommandBuffer); GET(EndCommandBuffer);
        GET(CmdPipelineBarrier); GET(CmdCopyImage); GET(QueueSubmit); GET(QueueSubmit2); GET(QueueSubmit2KHR); GET(GetDeviceQueue); GET(GetDeviceQueue2);
        GET(CreateFence); GET(DestroyFence); GET(WaitForFences); GET(GetFenceStatus); GET(WaitSemaphores); GET(WaitSemaphoresKHR); GET(GetSemaphoreCounterValue); GET(GetSemaphoreCounterValueKHR);
        GET(CreateImageView); GET(DestroyImageView); GET(CreateFramebuffer); GET(DestroyFramebuffer);
        GET(CmdBeginRenderPass); GET(CmdBeginRenderPass2); GET(CmdBeginRenderPass2KHR);
        GET(CmdClearColorImage); GET(CmdBeginRendering); GET(CmdBeginRenderingKHR); GET(CmdPipelineBarrier2); GET(CmdPipelineBarrier2KHR); GET(CmdExecuteCommands); GET(CmdDraw); GET(CmdDrawIndexed); GET(CmdCopyImage2); GET(CmdBlitImage2); GET(CmdResolveImage2); GET(CmdCopyBufferToImage2); GET(CmdCopyImage2KHR); GET(CmdBlitImage2KHR); GET(CmdResolveImage2KHR); GET(CmdCopyBufferToImage2KHR); GET(CreateRenderPass); GET(CreateRenderPass2); GET(CmdNextSubpass); GET(CmdEndRenderPass); GET(CmdBlitImage); GET(CmdCopyBufferToImage); GET(CmdResolveImage);
    }
    if (!worker) pthread_create(&worker, NULL, workerMain, NULL);
    pthread_mutex_unlock(&lock);
    if (wantFd) LOG("device %p: external_memory_fd over AHardwareBuffer", *out);
    return r;
}

static void DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a) {
    PFN_vkDestroyDevice f = (void *)realGDPA(dev, "vkDestroyDevice");
    pthread_mutex_lock(&lock); Dev *d = findDev(dev); if (d) d->dev = NULL; pthread_mutex_unlock(&lock);
    f(dev, a);
}

static void fixExternalProps(VkExternalMemoryProperties *p) {
    p->externalMemoryFeatures |= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                                 VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    p->exportFromImportedHandleTypes = OPAQUE; p->compatibleHandleTypes = OPAQUE;
}

static void GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out) {
    PFN_vkGetPhysicalDeviceExternalBufferProperties f = (void *)IPA("vkGetPhysicalDeviceExternalBufferProperties");
    if (!f) f = (void *)IPA("vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    VkPhysicalDeviceExternalBufferInfo i = *info;
    int op = i.handleType == OPAQUE;
    if (op) i.handleType = AHB;
    f(pd, &i, out);
    if (op && out->externalMemoryProperties.externalMemoryFeatures) fixExternalProps(&out->externalMemoryProperties);
}

// opaque-fd images are supported whenever the format is (layers and mips go through shadows)
// the guest driver passes the host's hostQueryReset feature through but has no vkResetQueryPool
static void GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *out) {
    PFN_vkGetPhysicalDeviceFeatures2 f = (void *)IPA("vkGetPhysicalDeviceFeatures2");
    if (!f) f = (void *)IPA("vkGetPhysicalDeviceFeatures2KHR");
    f(pd, out);
    VkPhysicalDeviceHostQueryResetFeatures *h = (void *)find(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
    if (h) h->hostQueryReset = VK_FALSE;
    VkPhysicalDeviceVulkan12Features *v = (void *)find(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    if (v) v->hostQueryReset = VK_FALSE;
    VkPhysicalDeviceLineRasterizationFeaturesEXT *l = (void *)find(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT);
    if (l) { memset(&l->rectangularLines, 0, 6 * sizeof(VkBool32)); l->bresenhamLines = VK_TRUE; }
}
// the depth resolve modes behind the claimed VK_KHR_depth_stencil_resolve
static void GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *out) {
    PFN_vkGetPhysicalDeviceProperties2 f = (void *)IPA("vkGetPhysicalDeviceProperties2");
    if (!f) f = (void *)IPA("vkGetPhysicalDeviceProperties2KHR");
    VkPhysicalDeviceDepthStencilResolveProperties *ds = (void *)find(out->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES);
    VkBaseOutStructure *l = ds ? unlink_(out, ds) : NULL;
    f(pd, out);
    relink(l, ds);
    if (ds && metaApp()) { ds->supportedDepthResolveModes = ds->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; ds->independentResolveNone = VK_TRUE; ds->independentResolve = VK_FALSE; }
}
static VkResult GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out) {
    PFN_vkGetPhysicalDeviceImageFormatProperties2 f = (void *)IPA("vkGetPhysicalDeviceImageFormatProperties2");
    if (!f) f = (void *)IPA("vkGetPhysicalDeviceImageFormatProperties2KHR");
    const VkPhysicalDeviceExternalImageFormatInfo *e = (const void *)find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    int op = e && e->handleType == OPAQUE;
    VkBaseOutStructure *l = op ? unlink_(info, e) : NULL;
    VkResult r = f(pd, info, out);
    relink(l, e);
    if (op && r == VK_SUCCESS) {
        VkExternalImageFormatProperties *p = (void *)find(out->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
        if (p) fixExternalProps(&p->externalMemoryProperties);
    }
    return r;
}

// ---- device level ----
// ---- multisampled eye targets ----
// Meta's Clay engine (ShellEnv) renders its eyes into a transient 4x multisampled target and records no resolve into
// the swapchain: on the Quest's tile GPU the multisampled data never leaves tile memory and lands in the swapchain
// on its own. On a desktop GPU nothing does that, so the target becomes a transfer source and, after each submit that
// rendered it, is resolved into the app's swapchain images of the same size, layers and format (which then go to the
// compositor like any other write).
typedef struct { VkImage im; VkDevice dev; VkExtent3D e; uint32_t layers; VkFormat fmt; VkCommandBuffer cb; uint32_t family; Shared *tgt[8]; int nt; } Msaa;
static Msaa msaa[16];
static struct { VkFramebuffer fb; Msaa *m; } fbMsaa[64];
static struct { VkCommandBuffer cb; Msaa *m; } cbMsaa[256];
static struct { VkImageView v; VkImage im; } viewImg[1024];
static Msaa *msaaOf(VkImage im) { for (int i = 0; i < 16; i++) if (im && msaa[i].im == im) return &msaa[i]; return NULL; }
static Msaa *cbMsaaGet(VkCommandBuffer cb) { for (int i = 0; i < 256; i++) if (cbMsaa[i].cb == cb) return cbMsaa[i].m; return NULL; }
static void cbMsaaSet(VkCommandBuffer cb, Msaa *m) {
    int f = -1; for (int i = 0; i < 256; i++) { if (cbMsaa[i].cb == cb) { cbMsaa[i].m = m; return; } if (f < 0 && !cbMsaa[i].cb) f = i; }
    if (f >= 0 && m) { cbMsaa[f].cb = cb; cbMsaa[f].m = m; }
}
// the resolve for one target set (rebuilt when the swapchain images change); call with the lock held
static VkCommandBuffer resolveCmd(Dev *d, Msaa *m, uint32_t family) {
    Shared *t[8]; int nt = 0;
    for (int i = 0; i < MAXSHARED && nt < 8; i++) {
        Shared *s = shared[i];
        if (s && s->dev == m->dev && s->complex && s->ready && s->ci.samples == VK_SAMPLE_COUNT_1_BIT && s->ci.format == m->fmt
            && s->ci.extent.width == m->e.width && s->ci.extent.height == m->e.height && s->ci.arrayLayers == m->layers) t[nt++] = s;
    }
    if (!nt) return VK_NULL_HANDLE;
    if (m->cb && m->family == family && m->nt == nt && !memcmp(m->tgt, t, nt * sizeof *t)) return m->cb;
    if (m->cb) d->FreeCommandBuffers(m->dev, d->pool[m->family], 1, &m->cb), m->cb = VK_NULL_HANDLE;
    if (family >= 8) return VK_NULL_HANDLE;
    if (!d->pool[family]) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (d->CreateCommandPool(m->dev, &pi, NULL, &d->pool[family])) return VK_NULL_HANDLE;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, d->pool[family], VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    if (d->AllocateCommandBuffers(m->dev, &ai, &cb)) return VK_NULL_HANDLE;
    *(void **)cb = *(void **)m->dev;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    d->BeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, m->layers};
    VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m->im, all};
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 1, &toSrc);
    VkImageResolve r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m->layers}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m->layers}, {0, 0, 0}, {m->e.width, m->e.height, 1}};
    for (int i = 0; i < nt; i++) d->CmdResolveImage(cb, m->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t[i]->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, NULL, 1, &back);
    d->EndCommandBuffer(cb);
    m->cb = cb; m->family = family; m->nt = nt; memcpy(m->tgt, t, nt * sizeof *t);
    LOG("resolving a %ux%u x%u multisampled target into %d swapchain images", m->e.width, m->e.height, m->layers, nt);
    return cb;
}

static VkResult CreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    Dev *d = findDev(dev);
    const VkExternalMemoryImageCreateInfo *e = (const void *)find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (!e && ci->samples != VK_SAMPLE_COUNT_1_BIT && (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && ci->imageType == VK_IMAGE_TYPE_2D && ci->mipLevels == 1) {
        VkImageCreateInfo c = *ci;
        c.usage = (c.usage & ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkResult r = d->CreateImage(dev, &c, a, out);
        if (!r) {
            pthread_mutex_lock(&lock);
            for (int i = 0; i < 16; i++) if (!msaa[i].im) { memset(&msaa[i], 0, sizeof msaa[i]); msaa[i].im = *out; msaa[i].dev = dev; msaa[i].e = ci->extent; msaa[i].layers = ci->arrayLayers; msaa[i].fmt = ci->format; break; }
            pthread_mutex_unlock(&lock);
        }
        return r;
    }
    if (e && (e->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)) {
        static unsigned n; if (n++ < 20) LOG("AHB image %ux%u x%u layers usage 0x%x", ci->extent.width, ci->extent.height, ci->arrayLayers, ci->usage); }
    if (ci->arrayLayers == 2 && (!e || !(e->handleTypes & OPAQUE))) {
        VkResult rr = d->CreateImage(dev, ci, a, out);
        static unsigned n; if (n++ < 20) LOG("plain image %p %ux%u x%u layers mips %u samples %u fmt %d usage 0x%x flags 0x%x ext 0x%x pnext %d", (void *)*out, ci->extent.width, ci->extent.height, ci->arrayLayers, ci->mipLevels, ci->samples, ci->format, ci->usage, ci->flags, e ? e->handleTypes : 0, ci->pNext ? ((const VkBaseInStructure *)ci->pNext)->sType : 0);
        return rr;
    }
    if (0) {
        VkResult rr = d->CreateImage(dev, ci, a, out);
        static unsigned n; if (n++ < 20) LOG("plain image %p %ux%u x%u layers mips %u samples %u fmt %d usage 0x%x flags 0x%x ext 0x%x", (void *)*out, ci->extent.width, ci->extent.height, ci->arrayLayers, ci->mipLevels, ci->samples, ci->format, ci->usage, ci->flags, e ? e->handleTypes : 0);
        return rr;
    }
    if (!e || !(e->handleTypes & OPAQUE)) return d->CreateImage(dev, ci, a, out);
    // Native Android buffers cannot carry storage-image usage on the Mac host driver.
    int complex = (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) || ci->arrayLayers > 1 || ci->mipLevels > 1 || ci->imageType != VK_IMAGE_TYPE_2D || ci->samples != VK_SAMPLE_COUNT_1_BIT
                  || ci->arrayLayers * ci->mipLevels > MAXSH;
    VkImageCreateInfo c = *ci; VkResult r;
    if (complex) {   // an ordinary image; sharing goes through shadows
        VkBaseOutStructure *l = unlink_(&c, e);
        r = d->CreateImage(dev, &c, a, out);
        relink(l, e);
    } else {         // backed directly by an AHardwareBuffer (no sRGB there: UNORM storage, sRGB views stay legal)
        VkExternalMemoryImageCreateInfo e2 = *e; e2.handleTypes = (e->handleTypes & ~OPAQUE) | AHB;
        VkBaseOutStructure *l = unlink_(&c, e);
        e2.pNext = c.pNext; c.pNext = &e2;
        if (ahbFormat(c.format) != c.format) { c.format = ahbFormat(c.format); c.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; }
        r = d->CreateImage(dev, &c, a, out);
        relink(l, e);
    }
    if (r == VK_SUCCESS) {
        Shared *s = calloc(1, sizeof *s);
        s->dev = dev; s->d = d; s->image = *out; s->ci = *ci; s->ci.pNext = NULL; s->ci.pQueueFamilyIndices = NULL;
        s->complex = complex; s->genFd = -1;
        pthread_mutex_lock(&lock); addShared(s); pthread_mutex_unlock(&lock);
    }
    LOG("external image %p: format %d %ux%u layers %u mips %u usage 0x%x %s -> %d", r ? NULL : (void *)*out, ci->format,
        ci->extent.width, ci->extent.height, ci->arrayLayers, ci->mipLevels, ci->usage, complex ? "shadowed" : "direct", r);
    return r;
}

static void DestroyImage(VkDevice dev, VkImage im, const VkAllocationCallbacks *a) {
    { Dev *dd = findDev(dev); pthread_mutex_lock(&lock); Msaa *m = msaaOf(im);
      if (m) { if (m->cb) dd->FreeCommandBuffers(dev, dd->pool[m->family], 1, &m->cb); memset(m, 0, sizeof *m); }
      pthread_mutex_unlock(&lock); }
    Dev *d = findDev(dev);
    pthread_mutex_lock(&lock); Shared *s = byImage(im); pthread_mutex_unlock(&lock);
    if (s) { if (d->lastExt == im) d->lastExt = VK_NULL_HANDLE; freeShared(s); }
    d->DestroyImage(dev, im, a);
}

static VkResult CreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    Dev *d = findDev(dev);
    VkExternalMemoryBufferCreateInfo *e = (void *)find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    VkExternalMemoryHandleTypeFlags was = e ? e->handleTypes : 0;
    if (was & OPAQUE) e->handleTypes = (was & ~OPAQUE) | AHB;
    VkResult r = d->CreateBuffer(dev, ci, a, out);
    if (e) e->handleTypes = was;
    return r;
}

static void GetImageMemoryRequirements2(VkDevice dev, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out) {
    Dev *d = findDev(dev);
    d->GetImageMemoryRequirements2(dev, info, out);
    if (byImage(info->image)) {
        VkMemoryDedicatedRequirements *dr = (void *)find(out->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
        if (dr) dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_TRUE;
        d->lastExt = info->image;
    }
}

static void GetImageMemoryRequirements(VkDevice dev, VkImage im, VkMemoryRequirements *out) {
    Dev *d = findDev(dev);
    d->GetImageMemoryRequirements(dev, im, out);
    if (byImage(im)) d->lastExt = im;
}

static VkResult AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    Dev *d = findDev(dev);
    const VkExportMemoryAllocateInfo *ex = (const void *)find(ai->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    const VkImportMemoryFdInfoKHR *im = (const void *)find(ai->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    const VkMemoryDedicatedAllocateInfo *di = (const void *)find(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    int exporting = ex && (ex->handleTypes & OPAQUE), importing = im && im->handleType == OPAQUE;
    if (!exporting && !importing) return d->AllocateMemory(dev, ai, a, out);
    VkImage target = di ? di->image : exporting ? d->lastExt : VK_NULL_HANDLE;
    pthread_mutex_lock(&lock); Shared *s = byImage(target); pthread_mutex_unlock(&lock);
    VkMemoryAllocateInfo m = *ai; VkResult r;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, target, VK_NULL_HANDLE};

    if (exporting) {
        VkExportMemoryAllocateInfo ex2 = *ex; ex2.handleTypes = (ex->handleTypes & ~OPAQUE) | AHB;
        VkBaseOutStructure *l = unlink_(&m, ex);
        if (!s || !s->complex) {   // direct: an AHardwareBuffer export (dedicated)
            ex2.pNext = m.pNext; m.pNext = &ex2;
            if (!di && target) { ded.pNext = m.pNext; m.pNext = &ded; }
        }
        r = d->AllocateMemory(dev, &m, a, out);
        relink(l, ex);
        if (r) { LOG("export allocate failed %d (image %p, %s)", r, (void *)target, s && s->complex ? "shadowed" : "direct"); return r; }
        if (target && d->lastExt == target) d->lastExt = VK_NULL_HANDLE;
        if (!s) return r;
        s->mem = *out; s->exporter = 1;
        if (!s->complex) { s->ready = 1; return r; }
        // shadows to share, and the generation counter. Depth isn't shared: the emulator has no depth Android buffers,
        // and the compositor needs only the colour (depth would feed optional depth reprojection). Each process keeps
        // its own depth image, so swapchains with depth (VrShell's overlay: the in-game menu) still get created.
        s->n = formatAspect(s->ci.format) == VK_IMAGE_ASPECT_COLOR_BIT ? s->ci.arrayLayers * s->ci.mipLevels : 0;
        s->genFd = memfd_create("macvr-gen", MFD_CLOEXEC);
        ftruncate(s->genFd, 4096);
        s->gen = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s->genFd, 0);
        for (uint32_t i = 0; i < s->n; i++)
            if ((r = makeShadow(s, i, NULL, NULL))) {
                LOG("export: shadow %u failed %d", i, r);
                d->FreeMemory(dev, *out, a);
                s->mem = VK_NULL_HANDLE;
                *out = VK_NULL_HANDLE;
                return r;
            }
        s->ready = 1;
        LOG("export %p: %u shadows", (void *)s->image, s->n);
        return VK_SUCCESS;
    }

    // importing
    Recv rv;
    if (recvBuffers(im->fd, &rv)) { LOG("import: fd %d carries no buffers", im->fd); return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
    VkBaseOutStructure *l = unlink_(&m, im);
    if (s && s->complex) {   // an ordinary allocation for the image; the shared data arrives through shadows
        VkMemoryRequirements mr; d->GetImageMemoryRequirements(dev, target, &mr);
        m.allocationSize = mr.size; m.memoryTypeIndex = pickType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        r = d->AllocateMemory(dev, &m, a, out);
        if (!r && rv.n != s->ci.arrayLayers * s->ci.mipLevels) LOG("import %p: %u buffers for %u layers x %u mips", (void *)target, rv.n, s->ci.arrayLayers, s->ci.mipLevels);
        if (!r) {
            s->mem = *out; s->n = rv.n < MAXSH ? rv.n : MAXSH;
            for (uint32_t i = 0; i < s->n && !r; i++) r = makeShadow(s, i, rv.buf[i], NULL);
            if (rv.genFd >= 0) { s->genFd = rv.genFd; rv.genFd = -1; s->gen = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s->genFd, 0); }
            s->ready = !r;
            if (r) LOG("import %p: shadow failed %d", (void *)target, r);
            else LOG("import %p: %u shadows", (void *)target, s->n);
        }
    } else if (!target && (rv.n > 1 || rv.genFd >= 0)) {   // shadowed, image not known yet: wait for the bind
        r = d->AllocateMemory(dev, &m, a, out);
        if (!r) {
            pthread_mutex_lock(&lock);
            int k = 0; while (k < 64 && pending[k].mem) k++;
            if (k < 64) { pending[k] = (Pending){dev, *out, rv}; rv.n = 0; rv.genFd = -1; }
            pthread_mutex_unlock(&lock);
            if (k == 64) LOG("too many pending imports");
        }
    } else {                 // direct: an AHardwareBuffer import
        VkImportAndroidHardwareBufferInfoANDROID ahb = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, m.pNext, rv.buf[0]};
        VkAndroidHardwareBufferPropertiesANDROID p = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        d->GetAndroidHardwareBufferPropertiesANDROID(dev, rv.buf[0], &p);
        m.pNext = &ahb;
        if (!(p.memoryTypeBits & (1u << m.memoryTypeIndex))) m.memoryTypeIndex = pickType(d, p.memoryTypeBits, 0);
        m.allocationSize = p.allocationSize;
        if (rv.n != 1) LOG("import: %u buffers into a direct image %p", rv.n, (void *)target);
        r = d->AllocateMemory(dev, &m, a, out);
        if (!r && s) { s->mem = *out; s->ready = 1; }
        if (r) LOG("import allocate failed %d", r);
    }
    relink(l, im);
    for (uint32_t i = 0; i < rv.n; i++) AHardwareBuffer_release(rv.buf[i]);
    if (rv.genFd >= 0) close(rv.genFd);
    if (r == VK_SUCCESS) close(im->fd);   // a successful import owns the fd
    return r;
}

static void FreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks *a) {
    Dev *d = findDev(dev);
    // ANGLE can import array memory without ever binding an image (its 3D external-storage
    // entry point is incomplete). Those deferred imports still own Android buffers and an fd.
    Recv orphan = {0}; orphan.genFd = -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 64; i++) if (pending[i].dev == dev && pending[i].mem == mem) {
        orphan = pending[i].rv;
        memset(&pending[i], 0, sizeof pending[i]);
        break;
    }
    pthread_mutex_unlock(&lock);
    for (uint32_t i = 0; i < orphan.n; i++) AHardwareBuffer_release(orphan.buf[i]);
    if (orphan.genFd >= 0) close(orphan.genFd);
    pthread_mutex_lock(&lock); Shared *s = byMem(mem); pthread_mutex_unlock(&lock);
    if (s) {   // drop the shadows with the memory; the image may get new memory later
        for (int f = 0; f < 8; f++) {
            if (s->in[f]) { d->FreeCommandBuffers(dev, d->pool[f], 1, &s->in[f]); s->in[f] = NULL; }
            if (s->out[f]) { d->FreeCommandBuffers(dev, d->pool[f], 1, &s->out[f]); s->out[f] = NULL; }
        }
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->sh[i]) d->DestroyImage(dev, s->sh[i], NULL);
            if (s->shm[i]) d->FreeMemory(dev, s->shm[i], NULL);
            s->sh[i] = NULL; s->shm[i] = NULL;
        }
        s->n = 0; s->mem = NULL; s->ready = 0;
    }
    d->FreeMemory(dev, mem, a);
}

static VkResult GetMemoryFdKHR(VkDevice dev, const VkMemoryGetFdInfoKHR *info, int *fd) {
    Dev *d = findDev(dev);
    pthread_mutex_lock(&lock); Shared *s = byMem(info->memory); pthread_mutex_unlock(&lock);
    AHardwareBuffer *bufs[MAXSH]; uint32_t n = 0; VkResult r = VK_SUCCESS;
    if (s && s->complex) {
        for (uint32_t i = 0; i < s->n && !r; i++) {
            VkMemoryGetAndroidHardwareBufferInfoANDROID gi = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, s->shm[i]};
            if (!(r = d->GetMemoryAndroidHardwareBufferANDROID(dev, &gi, &bufs[i]))) n++;
        }
    } else {
        VkMemoryGetAndroidHardwareBufferInfoANDROID gi = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, info->memory};
        if (!(r = d->GetMemoryAndroidHardwareBufferANDROID(dev, &gi, &bufs[0]))) n = 1;
    }
    if (!r) {
        *fd = sendBuffers(bufs, n, s ? s->ci.arrayLayers : 1, s ? s->ci.mipLevels : 1, s && s->complex ? s->genFd : -1);
        if (*fd < 0) r = VK_ERROR_TOO_MANY_OBJECTS;
    }
    for (uint32_t i = 0; i < n; i++) AHardwareBuffer_release(bufs[i]);
    if (r) LOG("vkGetMemoryFdKHR failed %d", r);
    return r;
}

static VkResult GetMemoryFdPropertiesKHR(VkDevice dev, VkExternalMemoryHandleTypeFlagBits type, int fd, VkMemoryFdPropertiesKHR *p) {
    Dev *d = findDev(dev);
    p->memoryTypeBits = (1u << d->mp.memoryTypeCount) - 1;
    return VK_SUCCESS;
}

static void bindPending(VkDevice dev, VkImage image, VkDeviceMemory mem) {
    { Shared *m = byMem(mem); if (m && m->image != image) { static unsigned n; if (n++ < 20) LOG("alias: image %p bound to the memory of shared %ux%u x%u (%p)", (void *)image, m->ci.extent.width, m->ci.extent.height, m->ci.arrayLayers, (void *)m->image); } }
    pthread_mutex_lock(&lock);
    Pending *p = NULL;
    for (int k = 0; k < 64; k++) if (pending[k].mem == mem && pending[k].dev == dev) p = &pending[k];
    Shared *s = p ? byImage(image) : NULL;
    Pending got = p ? *p : (Pending){0};
    if (p) memset(p, 0, sizeof *p);
    pthread_mutex_unlock(&lock);
    if (!p) return;
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (s && s->complex) {
        s->mem = mem; s->n = got.rv.n < MAXSH ? got.rv.n : MAXSH; r = VK_SUCCESS;
        for (uint32_t i = 0; i < s->n && !r; i++) r = makeShadow(s, i, got.rv.buf[i], NULL);
        if (got.rv.genFd >= 0) { s->genFd = got.rv.genFd; got.rv.genFd = -1; s->gen = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s->genFd, 0); }
        s->ready = !r;
    }
    LOG("bind %p: %u shadows from a pending import -> %d", (void *)image, got.rv.n, r);
    for (uint32_t i = 0; i < got.rv.n; i++) AHardwareBuffer_release(got.rv.buf[i]);
    if (got.rv.genFd >= 0) close(got.rv.genFd);
}
static VkResult BindImageMemory(VkDevice dev, VkImage im, VkDeviceMemory mem, VkDeviceSize off) {
    Dev *d = findDev(dev);
    VkResult r = d->BindImageMemory(dev, im, mem, off);
    if (!r) bindPending(dev, im, mem);
    return r;
}
static VkResult BindImageMemory2(VkDevice dev, uint32_t n, const VkBindImageMemoryInfo *bi) {
    Dev *d = findDev(dev);
    VkResult r = d->BindImageMemory2(dev, n, bi);
    for (uint32_t i = 0; !r && i < n; i++) bindPending(dev, bi[i].image, bi[i].memory);
    return r;
}

// ---- write tracking ----
static struct { VkImageView v; VkImage im; uint32_t base, count; VkImageViewType t; } dbgViews[512]; static unsigned dbgViewN;
static struct { VkImage im; VkExtent3D e; uint32_t layers, usage, ext; } dbgImgs[512]; static unsigned dbgImgN;
static VkResult CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    Dev *d = findDev(dev);
    VkResult r = d->CreateImageView(dev, ci, a, out);
    if (!r && msaaOf(ci->image)) { pthread_mutex_lock(&lock); for (int i = 0; i < 1024; i++) if (!viewImg[i].v || viewImg[i].v == *out) { viewImg[i].v = *out; viewImg[i].im = ci->image; break; } pthread_mutex_unlock(&lock); }
    if (!r) { unsigned k = __atomic_fetch_add(&dbgViewN, 1, __ATOMIC_RELAXED) % 512; dbgViews[k].v = *out; dbgViews[k].im = ci->image; dbgViews[k].base = ci->subresourceRange.baseArrayLayer; dbgViews[k].count = ci->subresourceRange.layerCount; dbgViews[k].t = ci->viewType; }
    pthread_mutex_lock(&lock);
    Shared *s = byImage(ci->image);
    if (!r && s && s->complex) { Track *t = slot(views, 256, (void *)*out, 1); if (!t) LOG("view table full"); addTo(t, s); }
    if (!r && ci->subresourceRange.layerCount > 1 || (!r && !s && ci->image)) { static unsigned m; if (m++ < 30 && !s) LOG("view %p on unshared image %p", (void *)*out, (void *)ci->image); }
    if (!r && s) { static unsigned n; if (n++ < 20) LOG("view %p on shared %ux%u complex %d", (void *)*out, s->ci.extent.width, s->ci.extent.height, s->complex); }
    pthread_mutex_unlock(&lock);
    return r;
}
static void DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *a) {
    Dev *d = findDev(dev);
    pthread_mutex_lock(&lock); Track *t = slot(views, 256, (void *)v, 0); if (t) memset(t->s, 0, sizeof t->s); pthread_mutex_unlock(&lock);
    d->DestroyImageView(dev, v, a);
}
static VkResult CreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) {
    Dev *d = findDev(dev);
    VkResult r = d->CreateFramebuffer(dev, ci, a, out);
    if (!r && ci->pAttachments) {
        pthread_mutex_lock(&lock);
        Msaa *m = NULL;
        for (uint32_t i = 0; i < ci->attachmentCount && !m; i++) for (int k = 0; k < 1024; k++) if (viewImg[k].v == ci->pAttachments[i]) { m = msaaOf(viewImg[k].im); break; }
        if (m) for (int i = 0; i < 64; i++) if (!fbMsaa[i].fb || fbMsaa[i].fb == *out) { fbMsaa[i].fb = *out; fbMsaa[i].m = m; break; }
        pthread_mutex_unlock(&lock);
    }
    if (r) LOG("vkCreateFramebuffer %ux%u (%u attachments) failed %d", ci->width, ci->height, ci->attachmentCount, r);
    if (ci->width == 1440) { static unsigned n; if (n++ < 10) {
        char b[256] = {0}; int o = 0;
        for (uint32_t i = 0; i < ci->attachmentCount && ci->pAttachments && o < 200; i++) {
            Track *v = slot(views, 256, (void *)ci->pAttachments[i], 0); VkImage im = 0; uint32_t bl = 0, lc = 0;
            for (unsigned k = 0; k < 512; k++) if (dbgViews[k].v == ci->pAttachments[i]) { im = dbgViews[k].im; bl = dbgViews[k].base; lc = dbgViews[k].count; }
            Shared *sh = byImage(im); Shared *sm = NULL;
            o += snprintf(b + o, sizeof b - o, " [view %p img %p layers %u+%u shared %d]", (void *)ci->pAttachments[i], (void *)im, bl, lc, sh != NULL); (void)v; (void)sm; }
        LOG("framebuffer %ux%ux%u flags 0x%x attachments %u%s", ci->width, ci->height, ci->layers, ci->flags, ci->attachmentCount, b); } }
    pthread_mutex_lock(&lock);
    if (!r && ci->pAttachments)
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            Track *v = slot(views, 256, (void *)ci->pAttachments[i], 0);
            if (v) for (int k = 0; k < 8; k++) addTo(slot(fbs, 256, (void *)*out, 1), v->s[k]);
            if (v && v->s[0]) { static unsigned n; if (n++ < 20) LOG("framebuffer %p: attachment %u is shared %ux%u", (void *)*out, i, v->s[0]->ci.extent.width, v->s[0]->ci.extent.height); }
        }
    if (!r && !ci->pAttachments) { static unsigned n; if (n++ < 5) LOG("framebuffer %p: imageless", (void *)*out); }
    pthread_mutex_unlock(&lock);
    return r;
}
static void DestroyFramebuffer(VkDevice dev, VkFramebuffer f, const VkAllocationCallbacks *a) {
    Dev *d = findDev(dev);
    pthread_mutex_lock(&lock); Track *t = slot(fbs, 256, (void *)f, 0); if (t) memset(t->s, 0, sizeof t->s); pthread_mutex_unlock(&lock);
    d->DestroyFramebuffer(dev, f, a);
}
static Dev *cbDev(VkCommandBuffer cb) {
    for (int i = 0; i < 16; i++) if (devs[i].dev && *(void **)devs[i].dev == *(void **)cb) return &devs[i];
    return NULL;
}
static void markPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 64; i++) if (fbMsaa[i].fb && fbMsaa[i].fb == bi->framebuffer) { cbMsaaSet(cb, fbMsaa[i].m); break; }
    Track *f = slot(fbs, 256, (void *)bi->framebuffer, 0);
    Shared *list[16]; int n = 0;
    if (f) for (int k = 0; k < 8; k++) if (f->s[k]) list[n++] = f->s[k];
    const VkRenderPassAttachmentBeginInfo *ab = (const void *)find(bi->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
    if (ab) for (uint32_t i = 0; i < ab->attachmentCount; i++) {
        Track *v = slot(views, 256, (void *)ab->pAttachments[i], 0);
        if (v) for (int k = 0; k < 8 && n < 16; k++) if (v->s[k]) list[n++] = v->s[k];
    }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) markCb(cb, list[i]);
}
static void CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) { markPass(cb, bi);
    { static unsigned n; if (n++ < 15) { Track *f = slot(fbs, 256, (void *)bi->framebuffer, 0); LOG("render pass %ux%u shared %d", bi->renderArea.extent.width, bi->renderArea.extent.height, f && f->s[0]); } } cbDev(cb)->CmdBeginRenderPass(cb, bi, c); }
static void CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *si) {
    markPass(cb, bi); Dev *d = cbDev(cb);
    { static unsigned n; if (n++ < 15) { Track *f = slot(fbs, 256, (void *)bi->framebuffer, 0); LOG("render pass2 %ux%u shared %d", bi->renderArea.extent.width, bi->renderArea.extent.height, f && f->s[0]); } }
    if (!d->CreateRenderPass2) d->CmdBeginRenderPass(cb, bi, si->contents);   // render passes are v1 underneath
    else (d->CmdBeginRenderPass2 ? d->CmdBeginRenderPass2 : d->CmdBeginRenderPass2KHR)(cb, bi, si);
}
// dynamic rendering (Vulkan 1.3; ShellEnv draws its eye buffers this way): the attachments it writes
static void markViews(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    Shared *list[16]; int n = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
        VkImageView vs[2] = {ri->pColorAttachments[i].imageView, ri->pColorAttachments[i].resolveImageView};
        for (int j = 0; j < 2; j++) { Track *v = vs[j] ? slot(views, 256, (void *)vs[j], 0) : NULL; if (v) for (int k = 0; k < 8 && n < 16; k++) if (v->s[k]) list[n++] = v->s[k]; }
    }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) markCb(cb, list[i]);
}
static void CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) { markViews(cb, ri);
    { static unsigned n; if (n++ < 15) LOG("dynamic rendering %ux%u views %u colors %u", ri->renderArea.extent.width, ri->renderArea.extent.height, ri->viewMask, ri->colorAttachmentCount); } Dev *d = cbDev(cb); (d->CmdBeginRendering ? d->CmdBeginRendering : d->CmdBeginRenderingKHR)(cb, ri); }
// Whatever command writes a shared image, the app moves it into or out of a writable layout around it: barriers
// are where writes are detected independent of the command used (copies, blits, resolves, render passes, compute).
static int writable(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || l == VK_IMAGE_LAYOUT_GENERAL
        || l == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
}
static void CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df, uint32_t nm,
                               const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    for (uint32_t i = 0; i < ni; i++) {
        Shared *sh = byImage(im[i].image);
        if (sh) { static unsigned n; if (n++ < 30) LOG("barrier %ux%u x%u: layout %d -> %d", sh->ci.extent.width, sh->ci.extent.height, sh->ci.arrayLayers, im[i].oldLayout, im[i].newLayout); }
        if (writable(im[i].newLayout) || writable(im[i].oldLayout)) markCb(cb, sh);
    }
    cbDev(cb)->CmdPipelineBarrier(cb, ss, ds, df, nm, m, nb, b, ni, im);
}
static void CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2 *im = &di->pImageMemoryBarriers[i];
        if (writable(im->newLayout) || writable(im->oldLayout)) markCb(cb, byImage(im->image));
    }
    Dev *d = cbDev(cb); (d->CmdPipelineBarrier2 ? d->CmdPipelineBarrier2 : d->CmdPipelineBarrier2KHR)(cb, di);
}
static unsigned draws;   // debug
static void CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { if (__atomic_add_fetch(&draws, 1, __ATOMIC_RELAXED) % 2000 == 1) LOG("draws: %u", draws); cbDev(cb)->CmdDraw(cb, a, b, c, d); }
static void CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { if (__atomic_add_fetch(&draws, 1, __ATOMIC_RELAXED) % 2000 == 1) LOG("draws: %u", draws); cbDev(cb)->CmdDrawIndexed(cb, a, b, c, d, e); }
// secondary command buffers: what they write, the primary that runs them writes
static void CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec) {
    Shared *list[64]; int k = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < n; i++) { Track *t = slot(cbs, MAXT, sec[i], 0); if (t) for (int j = 0; j < 8 && k < 64; j++) if (t->s[j]) list[k++] = t->s[j]; Msaa *m = cbMsaaGet(sec[i]); if (m) cbMsaaSet(cb, m); }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < k; i++) markCb(cb, list[i]);
    cbDev(cb)->CmdExecuteCommands(cb, n, sec);
}
// the Vulkan 1.3 forms of the transfer commands
#define V2(name, Info, dstField) static void name(VkCommandBuffer cb, const Info *i) { { static unsigned k; if (k++ < 10) LOG(#name " -> %p (shared %d)", (void *)i->dstField, byImage(i->dstField) != NULL); } markCb(cb, byImage(i->dstField)); Dev *d = cbDev(cb); (d->name ? d->name : d->name##KHR)(cb, i); }
V2(CmdCopyImage2, VkCopyImageInfo2, dstImage)
V2(CmdBlitImage2, VkBlitImageInfo2, dstImage)
V2(CmdResolveImage2, VkResolveImageInfo2, dstImage)
V2(CmdCopyBufferToImage2, VkCopyBufferToImageInfo2, dstImage)
static void CmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    markCb(cb, byImage(im)); cbDev(cb)->CmdClearColorImage(cb, im, l, c, n, r);
}
static void CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    { static unsigned k; if (k++ < 10) LOG("blit %p -> %p (shared %d)", (void *)s, (void *)dst, byImage(dst) != NULL); }
    markCb(cb, byImage(dst)); cbDev(cb)->CmdBlitImage(cb, s, sl, dst, dl, n, r, f);
}
static void CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    { static unsigned k; if (k++ < 10) LOG("copy %p -> %p (shared %d)", (void *)s, (void *)dst, byImage(dst) != NULL); }
    markCb(cb, byImage(dst)); cbDev(cb)->CmdCopyImage(cb, s, sl, dst, dl, n, r);
}
static void CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage dst, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) {
    markCb(cb, byImage(dst)); cbDev(cb)->CmdCopyBufferToImage(cb, b, dst, dl, n, r);
}
static void CmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    { static unsigned k; if (k++ < 10) LOG("resolve %p -> %p (shared %d)", (void *)s, (void *)dst, byImage(dst) != NULL); }
    markCb(cb, byImage(dst)); cbDev(cb)->CmdResolveImage(cb, s, sl, dst, dl, n, r);
}
static VkResult BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    pthread_mutex_lock(&lock); Track *t = slot(cbs, MAXT, cb, 0); if (t) memset(t->s, 0, sizeof t->s); cbMsaaSet(cb, NULL); pthread_mutex_unlock(&lock);
    return cbDev(cb)->BeginCommandBuffer(cb, bi);
}

static VkResult CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    { static unsigned n; if (n++ < 30) {
        char b[300] = {0}; int o = 0;
        for (uint32_t i = 0; i < ci->attachmentCount && o < 250; i++) o += snprintf(b + o, sizeof b - o, " a%u:fmt%d/s%d/ld%d/st%d/%d->%d", i, ci->pAttachments[i].format, ci->pAttachments[i].samples, ci->pAttachments[i].loadOp, ci->pAttachments[i].storeOp, ci->pAttachments[i].initialLayout, ci->pAttachments[i].finalLayout);
        LOG("renderpass %u att%s, %u subpasses (sp0 color %u input %u), resolve %d, pNext %d, sp0 pNext %d", ci->attachmentCount, b, ci->subpassCount, ci->pSubpasses[0].colorAttachmentCount, ci->pSubpasses[0].inputAttachmentCount,
            ci->pSubpasses[0].pResolveAttachments ? (int)ci->pSubpasses[0].pResolveAttachments[0].attachment : -1, ci->pNext ? ((const VkBaseInStructure *)ci->pNext)->sType : 0, 0); } }
    VkResult r = findDev(dev)->CreateRenderPass(dev, ci, a, out);
    if (r) LOG("vkCreateRenderPass (%u attachments, pNext %d) failed %d", ci->attachmentCount, ci->pNext ? ((const VkBaseInStructure *)ci->pNext)->sType : 0, r);
    return r;
}
// ---- VK_KHR_create_renderpass2 over render pass v1 (the emulator's driver has only v1) ----
static VkResult CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    Dev *d = findDev(dev);
    { static unsigned n; if (n++ < 30) {
        char b[300] = {0}; int o = 0;
        for (uint32_t i = 0; i < ci->attachmentCount && o < 250; i++) o += snprintf(b + o, sizeof b - o, " a%u:fmt%d/s%d/ld%d/st%d", i, ci->pAttachments[i].format, ci->pAttachments[i].samples, ci->pAttachments[i].loadOp, ci->pAttachments[i].storeOp);
        const VkSubpassDescription2 *sp = &ci->pSubpasses[0];
        LOG("renderpass2 %u att%s | subpass0 color %u resolve %d depthResolve %d viewMask 0x%x pNext %d", ci->attachmentCount, b, sp->colorAttachmentCount,
            sp->pResolveAttachments ? (int)sp->pResolveAttachments[0].attachment : -1,
            find(sp->pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE) != NULL, sp->viewMask, ci->pNext ? ((const VkBaseInStructure *)ci->pNext)->sType : 0); } }
    if (d->CreateRenderPass2) return d->CreateRenderPass2(dev, ci, a, out);
    VkAttachmentDescription *att = calloc(ci->attachmentCount + 1, sizeof *att);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        const VkAttachmentDescription2 *s2 = &ci->pAttachments[i];
        att[i] = (VkAttachmentDescription){s2->flags, s2->format, s2->samples, s2->loadOp, s2->storeOp, s2->stencilLoadOp, s2->stencilStoreOp, s2->initialLayout, s2->finalLayout};
    }
    uint32_t refs = 0;
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[i];
        refs += sp->inputAttachmentCount + sp->colorAttachmentCount * 2 + 1;
    }
    VkAttachmentReference *ref = calloc(refs + 1, sizeof *ref), *r = ref;
    VkSubpassDescription *sub = calloc(ci->subpassCount + 1, sizeof *sub);
    uint32_t *views = calloc(ci->subpassCount + 1, sizeof *views); int multiview = 0;
#define CONV(n, src) (src ? ({ VkAttachmentReference *b = r; for (uint32_t k = 0; k < (n); k++) *r++ = (VkAttachmentReference){(src)[k].attachment, (src)[k].layout}; b; }) : NULL)
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[i];
        sub[i].flags = sp->flags; sub[i].pipelineBindPoint = sp->pipelineBindPoint;
        sub[i].inputAttachmentCount = sp->inputAttachmentCount; sub[i].pInputAttachments = CONV(sp->inputAttachmentCount, sp->pInputAttachments);
        sub[i].colorAttachmentCount = sp->colorAttachmentCount; sub[i].pColorAttachments = CONV(sp->colorAttachmentCount, sp->pColorAttachments);
        sub[i].pResolveAttachments = CONV(sp->colorAttachmentCount, sp->pResolveAttachments);
        sub[i].pDepthStencilAttachment = CONV(1, sp->pDepthStencilAttachment);
        sub[i].preserveAttachmentCount = sp->preserveAttachmentCount; sub[i].pPreserveAttachments = sp->pPreserveAttachments;
        views[i] = sp->viewMask; multiview |= sp->viewMask != 0;
    }
    VkSubpassDependency *dep = calloc(ci->dependencyCount + 1, sizeof *dep);
    int32_t *offs = calloc(ci->dependencyCount + 1, sizeof *offs);
    for (uint32_t i = 0; i < ci->dependencyCount; i++) {
        const VkSubpassDependency2 *d2 = &ci->pDependencies[i];
        dep[i] = (VkSubpassDependency){d2->srcSubpass, d2->dstSubpass, d2->srcStageMask, d2->dstStageMask, d2->srcAccessMask, d2->dstAccessMask, d2->dependencyFlags};
        offs[i] = d2->viewOffset;
    }
    VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, ci->pNext, ci->subpassCount, views,
        ci->dependencyCount, offs, ci->correlatedViewMaskCount, ci->pCorrelatedViewMasks};
    VkRenderPassCreateInfo c = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, multiview ? (void *)&mv : (void *)ci->pNext, ci->flags,
        ci->attachmentCount, att, ci->subpassCount, sub, ci->dependencyCount, dep};
    VkResult res = d->CreateRenderPass(dev, &c, a, out);
    free(att); free(ref); free(sub); free(views); free(dep); free(offs);
    if (res) LOG("vkCreateRenderPass2 (as v1) failed %d", res);
    return res;
}
static void CmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo *b, const VkSubpassEndInfo *e) { cbDev(cb)->CmdNextSubpass(cb, b->contents); }
static void CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *e) { cbDev(cb)->CmdEndRenderPass(cb); }

// ---- queues and submits ----
typedef struct { VkQueue q; Dev *d; uint32_t family; } Q;
static Q queues[64];
static void addQueue(Dev *d, VkQueue q, uint32_t fam) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 64; i++) if (!queues[i].q || queues[i].q == q) { queues[i] = (Q){q, d, fam}; break; }
    pthread_mutex_unlock(&lock);
}
static void GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue *q) { Dev *d = findDev(dev); d->GetDeviceQueue(dev, fam, idx, q); addQueue(d, *q, fam); }
static void GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *qi, VkQueue *q) { Dev *d = findDev(dev); d->GetDeviceQueue2(dev, qi, q); addQueue(d, *q, qi->queueFamilyIndex); }

static VkResult QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    Q *q = NULL;
    for (int i = 0; i < 64; i++) if (queues[i].q == queue) { q = &queues[i]; break; }
    Dev *d = q ? q->d : cbDev((VkCommandBuffer)queue);
    if (!q) return d->QueueSubmit(queue, count, submits, fence);
    // shared images this submit writes (copied out after), and ones another process has written since (copied in)
    Shared *post[64], *pre[64]; int np = 0, nr = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t c = 0; c < submits[i].commandBufferCount; c++) {
            Track *t = slot(cbs, MAXT, submits[i].pCommandBuffers[c], 0);
            if (t) for (int k = 0; k < 8; k++) if (t->s[k] && (!t->s[k]->ready || t->s[k]->dev != d->dev)) {
                static unsigned n; if (n++ < 20) LOG("submit skips %ux%u: ready %d same device %d", t->s[k]->ci.extent.width, t->s[k]->ci.extent.height, t->s[k]->ready, t->s[k]->dev == d->dev); }
            if (t) for (int k = 0; k < 8; k++) if (t->s[k] && t->s[k]->ready && t->s[k]->dev == d->dev) {
                int dup = 0; for (int j = 0; j < np; j++) dup |= post[j] == t->s[k];
                if (!dup && np < 64) post[np++] = t->s[k];
            }
        }
    VkCommandBuffer res[8]; int nres = 0;
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t c = 0; c < submits[i].commandBufferCount; c++) {
            Msaa *m = cbMsaaGet(submits[i].pCommandBuffers[c]); VkCommandBuffer rc;
            if (m && nres < 8 && (rc = resolveCmd(d, m, q->family))) {
                int dupc = 0; for (int j = 0; j < nres; j++) dupc |= res[j] == rc;
                if (!dupc) res[nres++] = rc;
                for (int k = 0; k < m->nt; k++) { int dup = 0; for (int j = 0; j < np; j++) dup |= post[j] == m->tgt[k]; if (!dup && np < 64) post[np++] = m->tgt[k]; }
            }
        }
    for (int i = 0; i < MAXSHARED && nr < 64; i++) {
        Shared *s = shared[i];
        if (s && s->dev == d->dev && s->complex && s->ready && s->gen && atomic_load(s->gen) != s->seen) {
            s->seen = atomic_load(s->gen); pre[nr++] = s;
        }
    }
    pthread_mutex_unlock(&lock);
    if (!np && !nr) return d->QueueSubmit(queue, count, submits, fence);

    VkCommandBuffer preCb[64], postCb[72]; int a = 0, b = 0;
    for (int i = 0; i < nres; i++) postCb[b++] = res[i];
    for (int i = 0; i < nr; i++) { VkCommandBuffer c = copyCmd(pre[i], q->family, 1); if (c) preCb[a++] = c; }
    for (int i = 0; i < np; i++) { VkCommandBuffer c = copyCmd(post[i], q->family, 0); if (c) postCb[b++] = c; }
    for (int i = 0; i < np; i++) if (post[i]->ci.arrayLayers > 1) { static unsigned n; if (n++ % 500 == 0) LOG("stereo out #%u: %ux%u", n, post[i]->ci.extent.width, post[i]->ci.extent.height); }
    { static unsigned logged; if (logged < 40 && (np || nr)) { logged++;
        for (int i = 0; i < np; i++) LOG("submit: out %ux%u x%u layers", post[i]->ci.extent.width, post[i]->ci.extent.height, post[i]->ci.arrayLayers);
        for (int i = 0; i < nr; i++) LOG("submit: in %ux%u x%u layers", pre[i]->ci.extent.width, pre[i]->ci.extent.height, pre[i]->ci.arrayLayers); } }
    uint32_t n = count ? count : 1;
    VkSubmitInfo *si = calloc(n, sizeof *si);
    if (count) memcpy(si, submits, count * sizeof *si); else si[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkCommandBuffer *first = NULL, *last = NULL; VkPipelineStageFlags *masks = NULL;
    if (a) {   // shadows in before anything reads: the first batch, waiting on everything it waits on
        first = calloc(si[0].commandBufferCount + a, sizeof *first);
        memcpy(first, preCb, a * sizeof *first);
        memcpy(first + a, si[0].pCommandBuffers, si[0].commandBufferCount * sizeof *first);
        si[0].pCommandBuffers = first; si[0].commandBufferCount += a;
        if (si[0].waitSemaphoreCount) {
            masks = calloc(si[0].waitSemaphoreCount, sizeof *masks);
            for (uint32_t i = 0; i < si[0].waitSemaphoreCount; i++) masks[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            si[0].pWaitDstStageMask = masks;
        }
    }
    if (b) {   // shadows out after everything writes: the last batch
        uint32_t k = n - 1;
        last = calloc(si[k].commandBufferCount + b, sizeof *last);
        memcpy(last, si[k].pCommandBuffers, si[k].commandBufferCount * sizeof *last);
        memcpy(last + si[k].commandBufferCount, postCb, b * sizeof *last);
        si[k].pCommandBuffers = last; si[k].commandBufferCount += b;
    }
    VkResult r = d->QueueSubmit(queue, n, si, fence);
    free(si); free(first); free(last); free(masks);
    if (!r && b) {   // when the copies have run, tell the exporters
        Job *j = calloc(1, sizeof *j);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!d->CreateFence(d->dev, &fi, NULL, &j->fence) && !d->QueueSubmit(queue, 0, NULL, j->fence)) {
            j->d = d; j->n = np < 64 ? np : 64; memcpy(j->s, post, j->n * sizeof *post);
            pthread_mutex_lock(&lock); j->next = jobs; jobs = j; pthread_cond_signal(&jobCv); pthread_mutex_unlock(&lock);
        } else free(j);
    }
    return r;
}

// vkQueueSubmit2 (Vulkan 1.3; Meta's Clay engine submits this way): the same copies, as their own batches in queue
// order around the app's
static VkResult QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    Q *q = NULL;
    for (int i = 0; i < 64; i++) if (queues[i].q == queue) { q = &queues[i]; break; }
    Dev *d = q ? q->d : cbDev((VkCommandBuffer)queue);
    PFN_vkQueueSubmit2 real = d->QueueSubmit2 ? d->QueueSubmit2 : d->QueueSubmit2KHR;
    if (!q) return real(queue, count, submits, fence);
    Shared *post[64], *pre[64]; int np = 0, nr = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t c = 0; c < submits[i].commandBufferInfoCount; c++) {
            Track *t = slot(cbs, MAXT, submits[i].pCommandBufferInfos[c].commandBuffer, 0);
            if (t) for (int k = 0; k < 8; k++) if (t->s[k] && t->s[k]->ready && t->s[k]->dev == d->dev) {
                int dup = 0; for (int j = 0; j < np; j++) dup |= post[j] == t->s[k];
                if (!dup && np < 64) post[np++] = t->s[k];
            }
        }
    VkCommandBuffer res[8]; int nres = 0;
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t c = 0; c < submits[i].commandBufferInfoCount; c++) {
            Msaa *m = cbMsaaGet(submits[i].pCommandBufferInfos[c].commandBuffer); VkCommandBuffer rc;
            if (m && nres < 8 && (rc = resolveCmd(d, m, q->family))) {
                int dupc = 0; for (int j = 0; j < nres; j++) dupc |= res[j] == rc;
                if (!dupc) res[nres++] = rc;
                for (int k = 0; k < m->nt; k++) { int dup = 0; for (int j = 0; j < np; j++) dup |= post[j] == m->tgt[k]; if (!dup && np < 64) post[np++] = m->tgt[k]; }
            }
        }
    for (int i = 0; i < MAXSHARED && nr < 64; i++) {
        Shared *s = shared[i];
        if (s && s->dev == d->dev && s->complex && s->ready && s->gen && atomic_load(s->gen) != s->seen) { s->seen = atomic_load(s->gen); pre[nr++] = s; }
    }
    pthread_mutex_unlock(&lock);
    VkCommandBuffer preCb[64], postCb[72]; int a = 0, b = 0;
    for (int i = 0; i < nres; i++) postCb[b++] = res[i];
    for (int i = 0; i < nr; i++) { VkCommandBuffer c = copyCmd(pre[i], q->family, 1); if (c) preCb[a++] = c; }
    for (int i = 0; i < np; i++) { VkCommandBuffer c = copyCmd(post[i], q->family, 0); if (c) postCb[b++] = c; }
    if (a) { VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = a, .pCommandBuffers = preCb}; d->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE); }
    VkResult r = real(queue, count, submits, fence);
    if (!r && b) {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = b, .pCommandBuffers = postCb};
        Job *j = calloc(1, sizeof *j);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!d->CreateFence(d->dev, &fi, NULL, &j->fence) && !d->QueueSubmit(queue, 1, &si, j->fence)) {
            j->d = d; j->n = np; memcpy(j->s, post, j->n * sizeof *post);
            pthread_mutex_lock(&lock); j->next = jobs; jobs = j; pthread_cond_signal(&jobCv); pthread_mutex_unlock(&lock);
        } else free(j);
    }
    { static unsigned logged; if (logged < 20 && (np || nr)) { logged++; LOG("submit2: %d out, %d in", np, nr); } }
    return r;
}

// ---- dispatch ----
// Waits never block the host (debug.macvr.pollwaits=0 restores blocking ones): a host-side wait holds that guest thread's whole stream
// (it spins on the ring meanwhile), and one whose signaller died never returns. The guest asks for the status and
// sleeps between asks, 0.1 ms doubling to 0.5 ms.
static int pollWaits(void) {
    static int v = -1;
    if (v < 0) { char p[PROP_VALUE_MAX] = ""; __system_property_get("debug.macvr.pollwaits", p); v = strcmp(p, "0") != 0; }
    return v;
}
static uint64_t monoNs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static VkResult WaitForFences(VkDevice dev, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    Dev *d = findDev(dev);
    if (!pollWaits() || !d->GetFenceStatus) return d->WaitForFences(dev, n, f, all, timeout);
    uint64_t start = monoNs(); useconds_t nap = 100;
    for (;;) {
        uint32_t done = 0;
        for (uint32_t i = 0; i < n; i++) {
            VkResult r = d->GetFenceStatus(dev, f[i]);
            if (r == VK_SUCCESS) done++; else if (r != VK_NOT_READY) return r;
        }
        if (all ? done == n : done > 0) return VK_SUCCESS;
        if (!timeout || monoNs() - start >= timeout) return VK_TIMEOUT;
        usleep(nap); if (nap < 500) nap *= 2;
    }
}
static VkResult waitSemaphores(Dev *d, PFN_vkWaitSemaphores wait, PFN_vkGetSemaphoreCounterValue value, VkDevice dev,
                               const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    if (!pollWaits() || !value) return wait(dev, info, timeout);
    int any = info->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
    uint64_t start = monoNs(); useconds_t nap = 100;
    for (;;) {
        uint32_t done = 0;
        for (uint32_t i = 0; i < info->semaphoreCount; i++) {
            uint64_t v = 0; VkResult r = value(dev, info->pSemaphores[i], &v);
            if (r) return r;
            if (v >= info->pValues[i]) done++;
        }
        if (any ? done > 0 : done == info->semaphoreCount) return VK_SUCCESS;
        if (!timeout || monoNs() - start >= timeout) return VK_TIMEOUT;
        usleep(nap); if (nap < 500) nap *= 2;
    }
}
static VkResult WaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    Dev *d = findDev(dev); return waitSemaphores(d, d->WaitSemaphores, d->GetSemaphoreCounterValue, dev, info, timeout);
}
static VkResult WaitSemaphoresKHR(VkDevice dev, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    Dev *d = findDev(dev); return waitSemaphores(d, d->WaitSemaphoresKHR, d->GetSemaphoreCounterValueKHR, dev, info, timeout);
}
static PFN_vkVoidFunction GetDeviceProcAddr(VkDevice dev, const char *n);
#define HOOK(name, fn) if (!strcmp(n, name)) return (PFN_vkVoidFunction)fn
static PFN_vkVoidFunction deviceHook(const char *n) {
    HOOK("vkGetDeviceProcAddr", GetDeviceProcAddr);
    HOOK("vkDestroyDevice", DestroyDevice);
    HOOK("vkWaitForFences", WaitForFences);
    HOOK("vkWaitSemaphores", WaitSemaphores); HOOK("vkWaitSemaphoresKHR", WaitSemaphoresKHR);
    HOOK("vkCreateImage", CreateImage);
    HOOK("vkDestroyImage", DestroyImage);
    HOOK("vkCreateBuffer", CreateBuffer);
    HOOK("vkAllocateMemory", AllocateMemory);
    HOOK("vkFreeMemory", FreeMemory);
    HOOK("vkBindImageMemory", BindImageMemory);
    HOOK("vkBindImageMemory2", BindImageMemory2);
    HOOK("vkBindImageMemory2KHR", BindImageMemory2);
    HOOK("vkGetImageMemoryRequirements2", GetImageMemoryRequirements2);
    HOOK("vkGetImageMemoryRequirements2KHR", GetImageMemoryRequirements2);
    HOOK("vkGetImageMemoryRequirements", GetImageMemoryRequirements);
    HOOK("vkGetMemoryFdKHR", GetMemoryFdKHR);
    HOOK("vkGetMemoryFdPropertiesKHR", GetMemoryFdPropertiesKHR);
    HOOK("vkCreateImageView", CreateImageView);
    HOOK("vkDestroyImageView", DestroyImageView);
    HOOK("vkCreateFramebuffer", CreateFramebuffer);
    HOOK("vkDestroyFramebuffer", DestroyFramebuffer);
    HOOK("vkCmdBeginRenderPass", CmdBeginRenderPass);
    HOOK("vkCmdBeginRenderPass2", CmdBeginRenderPass2);
    HOOK("vkCmdBeginRenderPass2KHR", CmdBeginRenderPass2);
    HOOK("vkCmdClearColorImage", CmdClearColorImage);
    HOOK("vkCmdBeginRendering", CmdBeginRendering);
    HOOK("vkCmdExecuteCommands", CmdExecuteCommands);
    HOOK("vkCmdDraw", CmdDraw); HOOK("vkCmdDrawIndexed", CmdDrawIndexed);
    HOOK("vkCmdCopyImage2", CmdCopyImage2); HOOK("vkCmdCopyImage2KHR", CmdCopyImage2);
    HOOK("vkCmdBlitImage2", CmdBlitImage2); HOOK("vkCmdBlitImage2KHR", CmdBlitImage2);
    HOOK("vkCmdResolveImage2", CmdResolveImage2); HOOK("vkCmdResolveImage2KHR", CmdResolveImage2);
    HOOK("vkCmdCopyBufferToImage2", CmdCopyBufferToImage2); HOOK("vkCmdCopyBufferToImage2KHR", CmdCopyBufferToImage2);
    HOOK("vkCmdPipelineBarrier", CmdPipelineBarrier);
    HOOK("vkCmdPipelineBarrier2", CmdPipelineBarrier2);
    HOOK("vkCmdPipelineBarrier2KHR", CmdPipelineBarrier2);
    HOOK("vkCmdBeginRenderingKHR", CmdBeginRendering);
    HOOK("vkCmdBlitImage", CmdBlitImage);
    HOOK("vkCmdCopyImage", CmdCopyImage);
    HOOK("vkCmdCopyBufferToImage", CmdCopyBufferToImage);
    HOOK("vkCmdResolveImage", CmdResolveImage);
    HOOK("vkBeginCommandBuffer", BeginCommandBuffer);
    HOOK("vkGetDeviceQueue", GetDeviceQueue);
    HOOK("vkGetDeviceQueue2", GetDeviceQueue2);
    HOOK("vkQueueSubmit", QueueSubmit);
    HOOK("vkQueueSubmit2", QueueSubmit2);
    HOOK("vkQueueSubmit2KHR", QueueSubmit2);
    HOOK("vkCreateRenderPass", CreateRenderPass);
    HOOK("vkCreateRenderPass2", CreateRenderPass2);
    HOOK("vkCreateRenderPass2KHR", CreateRenderPass2);
    HOOK("vkCmdNextSubpass2", CmdNextSubpass2);
    HOOK("vkCmdNextSubpass2KHR", CmdNextSubpass2);
    HOOK("vkCmdEndRenderPass2", CmdEndRenderPass2);
    HOOK("vkCmdEndRenderPass2KHR", CmdEndRenderPass2);
    return NULL;
}
static PFN_vkVoidFunction GetDeviceProcAddr(VkDevice dev, const char *n) {
    if (!realGDPA) realGDPA = (PFN_vkGetDeviceProcAddr)IPA("vkGetDeviceProcAddr");
    if (!strncmp(n, "vkCmd", 5)) { static unsigned k; if (k++ < 400) LOG("gdpa %s", n); }
    PFN_vkVoidFunction h = deviceHook(n);
    return h ? h : realGDPA(dev, n);
}
static VkResult CreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static PFN_vkVoidFunction GetInstanceProcAddr(VkInstance inst, const char *n) {
    if (inst) gInst = inst;
    HOOK("vkCreateInstance", CreateInstance);
    HOOK("vkGetInstanceProcAddr", GetInstanceProcAddr);
    HOOK("vkEnumerateDeviceExtensionProperties", EnumerateDeviceExtensionProperties);
    HOOK("vkCreateDevice", CreateDevice);
    HOOK("vkGetPhysicalDeviceExternalBufferProperties", GetPhysicalDeviceExternalBufferProperties);
    HOOK("vkGetPhysicalDeviceExternalBufferPropertiesKHR", GetPhysicalDeviceExternalBufferProperties);
    HOOK("vkGetPhysicalDeviceImageFormatProperties2", GetPhysicalDeviceImageFormatProperties2);
    HOOK("vkGetPhysicalDeviceFeatures2", GetPhysicalDeviceFeatures2);
    HOOK("vkGetPhysicalDeviceProperties2", GetPhysicalDeviceProperties2);
    HOOK("vkGetPhysicalDeviceProperties2KHR", GetPhysicalDeviceProperties2);
    HOOK("vkGetPhysicalDeviceFeatures2KHR", GetPhysicalDeviceFeatures2);
    HOOK("vkGetPhysicalDeviceImageFormatProperties2KHR", GetPhysicalDeviceImageFormatProperties2);
    PFN_vkVoidFunction h = deviceHook(n);
    if (h) return h;
    return realGIPA(inst, n);
}

static VkResult CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkResult r = realDev->CreateInstance(ci, a, out);
    if (r == VK_SUCCESS) gInst = *out;
    else {
        uint32_t n = 0; realDev->EnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *p = calloc(n, sizeof *p); realDev->EnumerateInstanceExtensionProperties(NULL, &n, p);
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
            int ok = 0; for (uint32_t j = 0; j < n; j++) if (!strcmp(p[j].extensionName, ci->ppEnabledExtensionNames[i])) ok = 1;
            if (!ok) LOG("vkCreateInstance %d: driver lacks %s", r, ci->ppEnabledExtensionNames[i]);
        }
        free(p);
    }
    return r;
}

// ---- HAL module ----
static int closeDevice(struct hw_device_t *d) { return 0; }
static hwvulkan_device_t ours;
static int openDevice(const struct hw_module_t *m, const char *id, struct hw_device_t **out) {
    if (!realDev) {
        void *lib = dlopen("/vendor/lib64/hw/vulkan.ranchu.so", RTLD_NOW | RTLD_LOCAL);
        hwvulkan_module_t *rm = lib ? dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR) : NULL;
        if (!rm || rm->common.methods->open(&rm->common, id, (struct hw_device_t **)&realDev) != 0) { LOG("can't open vulkan.ranchu"); return -1; }
        realGIPA = realDev->GetInstanceProcAddr;
    }
    ours.common = realDev->common;
    ours.common.module = (struct hw_module_t *)m;
    ours.common.close = closeDevice;
    ours.EnumerateInstanceExtensionProperties = realDev->EnumerateInstanceExtensionProperties;
    ours.CreateInstance = CreateInstance;
    ours.GetInstanceProcAddr = GetInstanceProcAddr;
    *out = &ours.common;
    return 0;
}
static struct hw_module_methods_t methods = {.open = openDevice};
__attribute__((visibility("default"))) hwvulkan_module_t HAL_MODULE_INFO_SYM = {
    .common = {.tag = HARDWARE_MODULE_TAG, .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
               .hal_api_version = HARDWARE_HAL_API_VERSION, .id = HWVULKAN_HARDWARE_MODULE_ID,
               .name = "MacVR Vulkan (emulator + external_memory_fd)", .author = "MacVR", .methods = &methods},
};
