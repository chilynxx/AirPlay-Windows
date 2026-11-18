// Decision logic of the automatic session reset (see airplay/link_watchdog.h).

#include "airplay/link_watchdog.h"

#include <cstdint>
#include <cstdio>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

constexpr int64_t kSec = 1'000'000'000LL;

int main() {
    using ap::airplay::LinkWatchdog;
    {
        LinkWatchdog w;
        check(!w.poll(false, 0, 100 * kSec, 5), "no link: nothing to reset");
        check(!w.poll(true, 100 * kSec, 102 * kSec, 5), "data 2 s ago, limit 5 s: keep waiting");
        check(!w.poll(true, 100 * kSec, 105 * kSec, 5), "exactly at the limit: keep waiting");
        check( w.poll(true, 100 * kSec, 105 * kSec + 1, 5), "silent for more than the limit: reset");
        check(!w.poll(true, 100 * kSec, 130 * kSec, 5), "reset fires once per dead link");
        check(!w.poll(false, 100 * kSec, 131 * kSec, 5), "link closed: re-armed");
        check(!w.poll(true, 140 * kSec, 142 * kSec, 5), "a new link starts healthy");
        check( w.poll(true, 140 * kSec, 150 * kSec, 5), "and is reset when it dies too");
    }
    {
        LinkWatchdog w;
        check(!w.poll(true, 0, 1000 * kSec, 0), "limit 0 switches the watchdog off");
        check(!w.poll(true, 0, 1000 * kSec, -3), "negative limit also means off");
    }
    {
        LinkWatchdog w;
        check(w.poll(true, 0, 6 * kSec, 5), "a silent link triggers before disabling");
        check(!w.poll(true, 0, 7 * kSec, 0), "disabling watchdog clears its fired state");
        check(w.poll(true, 0, 7 * kSec, 5), "re-enabling watchdog can supervise the same active link");
    }
    {
        LinkWatchdog w;
        check(!w.poll(true, 200 * kSec, 100 * kSec, 5), "clock values moving backwards do not trigger recovery");
        check(w.poll(true, INT64_MIN, INT64_MAX, 1), "extreme timestamps do not overflow elapsed-time arithmetic");
    }
    {
        LinkWatchdog w;
        // Data keeps arriving: never fires, however long the session lasts.
        bool fired = false;
        for (int64_t t = 1; t <= 4 * 3600; ++t) fired |= w.poll(true, t * kSec, t * kSec + kSec / 2, 3);
        check(!fired, "four hours of steady data never trigger a reset");
    }
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
