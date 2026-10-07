// vtenc: the stream's frame path in native code, so it doesn't take the emulator's performance cores.
//
// Follows the emulator display that bridge/grpc_follow.py receives into a memory-mapped file (portrait RGBA, a frame
// counter in <file>.seq), and for each new frame: rotates it upright, reads the compositor's pose-number stamp,
// skips repeats, covers the stamp, converts to NV12 (BT.601, video range) straight into an IOSurface pixel buffer and
// encodes it with VideoToolbox's low-latency H.264 encoder. Records go to stdout:
//   1 u32 len, i64 stamp (-1: unstamped), u8 keyframe, len bytes of Annex B H.264 (parameter sets before keyframes)
//   2 i64 last stamp seen, u32 frames seen, u32 frames encoded (once a second)
// stdin takes lines: "idr", "gain <0..1>" (Horizon's brightness, applied to luma and chroma), "idle <0|1>" (no
// tracking: one frame a second), "quit". Usage: vtenc <frame file> <w> <h> <fps> <bitrate>  (w x h: upright size)
#include <Accelerate/Accelerate.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MARK 8
#define MARK_BLOCKS 36
// one thread per call: vImage's own tiling across cores spun more CPU than it saved at this size
#define TILE kvImageDoNotTile

static pthread_mutex_t outLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int wantIdr = 1, idle = 0, quitting = 0;
static _Atomic float gain = 1.0f;
static dispatch_semaphore_t inflight;
static uint32_t encoded;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void writeAll(const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t k = write(1, c, n); if (k <= 0) { atomic_store(&quitting, 1); return; } c += k; n -= (size_t)k; }
}

static void output(void *ref, void *frameRef, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    int64_t stamp = (int64_t)(intptr_t)frameRef - 1;
    if (status || !sb || (flags & kVTEncodeInfo_FrameDropped)) { dispatch_semaphore_signal(inflight); return; }
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    int key = !(att && CFArrayGetCount(att) &&
                CFDictionaryContainsKey(CFArrayGetValueAtIndex(att, 0), kCMSampleAttachmentKey_NotSync));
    static const uint8_t start[4] = {0, 0, 0, 1};
    size_t cap = CMSampleBufferGetTotalSampleSize(sb) + 1024, len = 0;
    uint8_t *buf = malloc(cap);
    if (key) {   // the parameter sets, so a decoder can join at any keyframe
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
        size_t count = 0; CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &count, NULL);
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ps; size_t n;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &ps, &n, NULL, NULL)) continue;
            if (len + 4 + n > cap) buf = realloc(buf, cap = len + 4 + n + 1024);
            memcpy(buf + len, start, 4); memcpy(buf + len + 4, ps, n); len += 4 + n;
        }
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t total = CMBlockBufferGetDataLength(bb), off = 0;
    if (len + total > cap) buf = realloc(buf, cap = len + total);
    CMBlockBufferCopyDataBytes(bb, 0, total, buf + len);
    while (off + 4 <= total) {   // AVCC lengths -> start codes
        uint8_t *p = buf + len + off;
        uint32_t n = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        memcpy(p, start, 4); off += 4 + n;
    }
    len += total;
    uint8_t type = 1, k = (uint8_t)key; uint32_t l = (uint32_t)len;
    pthread_mutex_lock(&outLock);
    writeAll(&type, 1); writeAll(&l, 4); writeAll(&stamp, 8); writeAll(&k, 1); writeAll(buf, len);
    encoded++;
    pthread_mutex_unlock(&outLock);
    free(buf);
    dispatch_semaphore_signal(inflight);
}

static void *commands(void *u) {
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "idr", 3)) atomic_store(&wantIdr, 1);
        else if (!strncmp(line, "gain ", 5)) atomic_store(&gain, (float)atof(line + 5));
        else if (!strncmp(line, "idle ", 5)) atomic_store(&idle, atoi(line + 5));
        else if (!strncmp(line, "quit", 4)) break;
    }
    atomic_store(&quitting, 1);   // stdin closed: the streamer is gone
    return NULL;
}

// the stamp the compositor writes along the bottom-left edge (green bits 1011, then a 32-bit pose number), or -1
static int64_t readStamp(const uint8_t *rgba, int w, int h) {
    const uint8_t *row = rgba + (size_t)(h - MARK / 2) * w * 4;
    int bits[MARK_BLOCKS];
    for (int i = 0; i < MARK_BLOCKS; i++) bits[i] = row[(MARK / 2 + i * MARK) * 4 + 1] > 127;
    if (!(bits[0] && !bits[1] && bits[2] && bits[3])) return -1;
    int64_t v = 0;
    for (int i = 4; i < MARK_BLOCKS; i++) v |= (int64_t)bits[i] << (i - 4);
    return v;
}

static uint64_t signature(const uint8_t *rgba, int w, int h) {   // FNV-1a over every 16th pixel
    uint64_t x = 1469598103934665603ull;
    for (int y = 0; y < h; y += 16) for (int i = 0; i < w; i += 16) {
        uint32_t p; memcpy(&p, rgba + ((size_t)y * w + i) * 4, 4); x = (x ^ p) * 1099511628211ull;
    }
    return x;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: vtenc <frame file> <w> <h> <fps> <bitrate>\n"); return 2; }
    const char *path = argv[1]; int W = atoi(argv[2]), H = atoi(argv[3]), fps = atoi(argv[4]), bitrate = atoi(argv[5]);
    char seqPath[1024]; snprintf(seqPath, sizeof seqPath, "%s.seq", path);
    int fd = open(path, O_RDONLY), sfd = open(seqPath, O_RDONLY);
    if (fd < 0 || sfd < 0) { perror("frame file"); return 1; }
    const uint8_t *raw = mmap(NULL, (size_t)W * H * 4, PROT_READ, MAP_SHARED, fd, 0);   // portrait: H wide, W tall
    const volatile uint64_t *seq = mmap(NULL, 8, PROT_READ, MAP_SHARED, sfd, 0);
    if (raw == MAP_FAILED || seq == MAP_FAILED) { perror("mmap"); return 1; }

    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);
    VTCompressionSessionRef s;
    if (VTCompressionSessionCreate(0, W, H, kCMVideoCodecType_H264, spec, 0, 0, output, 0, &s)) { fprintf(stderr, "no hardware H.264 encoder\n"); return 1; }
    CFNumberRef n;
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel);
    n = CFNumberCreate(0, kCFNumberIntType, &bitrate); VTSessionSetProperty(s, kVTCompressionPropertyKey_AverageBitRate, n);
    n = CFNumberCreate(0, kCFNumberIntType, &fps); VTSessionSetProperty(s, kVTCompressionPropertyKey_ExpectedFrameRate, n);
    int gop = 10 * fps; n = CFNumberCreate(0, kCFNumberIntType, &gop); VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, n);
    VTCompressionSessionPrepareToEncodeFrames(s);

    vImage_YpCbCrPixelRange range = {16, 128, 235, 240, 235, 16, 240, 16};   // video range
    vImage_ARGBToYpCbCr info;
    vImageConvert_ARGBToYpCbCr_GenerateConversion(kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4, &range, &info,
                                                  kvImageARGB8888, kvImage420Yp8_CbCr8, kvImageNoFlags);
    const uint8_t rgbaAsArgb[4] = {3, 0, 1, 2};
    uint8_t *upright = malloc((size_t)W * H * 4);
    CFDictionaryRef ios = CFDictionaryCreate(0, 0, 0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ak[] = {kCVPixelBufferIOSurfacePropertiesKey}, *av[] = {ios};
    CFDictionaryRef attrs = CFDictionaryCreate(0, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferPoolRef pool;
    const void *pk[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
    int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const void *pv[] = {CFNumberCreate(0, kCFNumberIntType, &fmt), CFNumberCreate(0, kCFNumberIntType, &W), CFNumberCreate(0, kCFNumberIntType, &H), ios};
    CVPixelBufferPoolCreate(0, NULL, CFDictionaryCreate(0, pk, pv, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks), &pool);
    (void)attrs;

    inflight = dispatch_semaphore_create(2);
    pthread_t cmd; pthread_create(&cmd, NULL, commands, NULL);
    uint64_t last = *seq, lastSig = 0; int64_t lastStamp = -2, seenStamp = -1;
    double lastEncode = 0, lastStat = now(); uint32_t seen = 0; float lastGain = -1;
    uint8_t lutY[256], lutC[256];
    while (!atomic_load(&quitting)) {
        double t = now();
        if (t - lastStat >= 1) {
            uint8_t type = 2; pthread_mutex_lock(&outLock);
            writeAll(&type, 1); writeAll(&seenStamp, 8); writeAll(&seen, 4); writeAll(&encoded, 4);
            pthread_mutex_unlock(&outLock); lastStat = t;
        }
        uint64_t cur = *seq;
        if (cur == last) { usleep(500); continue; }
        last = cur;
        vImage_Buffer src = {(void *)raw, (vImagePixelCount)W, (vImagePixelCount)H, (size_t)H * 4};   // portrait
        vImage_Buffer dst = {upright, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4};
        uint8_t black[4] = {0};
        vImageRotate90_ARGB8888(&src, &dst, kRotate90DegreesClockwise, black, TILE);
        if (*seq != cur) continue;   // written while copying: take the next one
        seen++;
        int64_t stamp = readStamp(upright, W, H);
        if (stamp >= 0) seenStamp = stamp;
        uint64_t sig = signature(upright, W, H);
        int wait = atomic_load(&idle) || stamp < 0 || (stamp == lastStamp && sig == lastSig);
        if (wait && t - lastEncode < 1) continue;   // repeats, unstamped or untracked frames: one a second
        // cover the stamp with the row above it (black there shows when the headset stretches the edge)
        for (int y = H - MARK; y < H; y++) memcpy(upright + (size_t)y * W * 4, upright + (size_t)(H - MARK - 1) * W * 4, MARK_BLOCKS * MARK * 4);
        CVPixelBufferRef pb;
        if (CVPixelBufferPoolCreatePixelBuffer(0, pool, &pb)) continue;
        CVPixelBufferLockBaseAddress(pb, 0);
        vImage_Buffer argb = {upright, (vImagePixelCount)H, (vImagePixelCount)W, (size_t)W * 4};
        vImage_Buffer y = {CVPixelBufferGetBaseAddressOfPlane(pb, 0), (vImagePixelCount)H, (vImagePixelCount)W, CVPixelBufferGetBytesPerRowOfPlane(pb, 0)};
        vImage_Buffer c = {CVPixelBufferGetBaseAddressOfPlane(pb, 1), (vImagePixelCount)H / 2, (vImagePixelCount)W / 2, CVPixelBufferGetBytesPerRowOfPlane(pb, 1)};
        vImageConvert_ARGB8888To420Yp8_CbCr8(&argb, &y, &c, &info, rgbaAsArgb, TILE);
        float g = atomic_load(&gain);
        if (g < 0.995f) {
            if (g != lastGain) {
                for (int i = 0; i < 256; i++) {
                    float a = 16 + (i - 16) * g, b = 128 + (i - 128) * g;
                    lutY[i] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a); lutC[i] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
                }
                lastGain = g;
            }
            vImageTableLookUp_Planar8(&y, &y, lutY, kvImageNoFlags);
            vImage_Buffer cb = {c.data, c.height, c.width * 2, c.rowBytes};
            vImageTableLookUp_Planar8(&cb, &cb, lutC, kvImageNoFlags);
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        CFDictionaryRef props = NULL;
        if (atomic_exchange(&wantIdr, 0)) {
            const void *k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame}, *v[] = {kCFBooleanTrue};
            props = CFDictionaryCreate(0, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        VTCompressionSessionEncodeFrame(s, pb, CMTimeMake((int64_t)(t * 1e6), 1000000), kCMTimeInvalid, props,
                                        (void *)(intptr_t)(stamp + 1), NULL);
        if (props) CFRelease(props);
        CVPixelBufferRelease(pb);
        lastStamp = stamp; lastSig = sig; lastEncode = t;
    }
    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    return 0;
}
