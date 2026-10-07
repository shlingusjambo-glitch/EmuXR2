#include "../vsync_schedule.h"
#include <assert.h>
#include <stdio.h>
int main() {
    const int64_t p = 1000000000LL / 72;
    assert(latestVsync(p, p-1, p) == 0);
    assert(latestVsync(p, p, p) == p);
    assert(latestVsync(p, p+500000, p) == p);
    assert(latestVsync(p, p*7+500000, p) == p*7);
    int64_t deadline=p, previous=0;
    for (int i=0;i<10000;i++) {
        int64_t now=deadline+(i%13)*p+12345;
        int64_t tick=latestVsync(deadline,now,p);
        assert(tick>previous && tick<=now && now-tick<p);
        previous=tick;deadline=tick+p;
    }
    assert(latestVsync(0,100,0)==0);
    puts("PASS: delayed vsync skips stale ticks, stays monotonic and never future-dated");
}
