#pragma once
#include <stdint.h>
// A delayed wake represents the latest completed tick, never a burst of stale ticks.
static inline int64_t latestVsync(int64_t deadline, int64_t now, int64_t period) {
    if (period <= 0 || now < deadline) return 0;
    return deadline + ((now - deadline) / period) * period;
}
