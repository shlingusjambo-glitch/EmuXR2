// Display vsync for Meta's compositor. On the Quest it reads the panel's last vsync time from the display driver
// (/sys/class/drm/sde-crtc-0/vsync_event, "VSYNC=<CLOCK_MONOTONIC ns>", read with pread). The emulator has no such
// node: macvr-hal.rc mounts a tmpfs over /sys/class/drm with that file, and this thread keeps it current at the
// composer's refresh rate.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <log/log.h>
int32_t composerRate();
void registerVsync() {
    int fd = open("/sys/class/drm/sde-crtc-0/vsync_event", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { ALOGE("vsync: no vsync_event node (%s)", strerror(errno)); return; }
    std::thread([fd] {
        timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        for (;;) {
            int64_t period = 1000000000LL / (composerRate() > 0 ? composerRate() : 72);
            int64_t ns = t.tv_sec * 1000000000LL + t.tv_nsec + period;
            t.tv_sec = ns / 1000000000LL; t.tv_nsec = ns % 1000000000LL;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
            // fixed width (space padded: the reader's strtoll uses base 0, so no leading zeros)
            char s[40]; int n = snprintf(s, sizeof s, "VSYNC=%20lld\n", (long long)ns);
            pwrite(fd, s, n, 0);
        }
    }).detach();
    ALOGI("vsync: running");
}
