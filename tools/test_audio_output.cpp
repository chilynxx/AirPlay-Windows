// Sound-device watchdog test using SDL's "dummy" audio driver (no hardware
// needed): normal consumption must never look like a stall, and reopen() must
// leave a working device behind.

#define SDL_MAIN_HANDLED   // keep our own main() when SDL.h is pulled in on Windows
#include "audio/audio_output.h"

#include <SDL.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main() {
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    ap::audio::SdlAudioOutput out;
    if (!out.start(44100, 2)) {
        std::printf("SKIP: no SDL audio driver available\n");
        return 0;
    }

    std::vector<int16_t> one_second(44100 * 2, 1000);
    out.push(one_second.data(), static_cast<int>(one_second.size()));
    check(out.queued_bytes() > 0, "audio queued");

    // The dummy device plays in real time: while it does, there is no stall.
    bool ever_stalled = false;
    for (int i = 0; i < 14; ++i) {
        std::this_thread::sleep_for(50ms);
        if (out.stalled(400ms)) ever_stalled = true;
    }
    check(!ever_stalled, "steady consumption is never reported as a stall");

    out.clear();
    check(!out.stalled(0ms), "an empty queue is never a stall");

    check(out.reopen(), "reopen() succeeds");
    out.push(one_second.data(), static_cast<int>(one_second.size()));
    check(out.queued_bytes() > 0, "device accepts audio after reopen");
    check(out.pending_ns() > 0, "pending time is reported after reopen");

    out.stop();
    out.stop();   // idempotent
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
