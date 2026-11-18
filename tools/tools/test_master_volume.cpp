// PC-side volume: range, mute and the perceptual curve.

#include "audio/master_volume.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main() {
    auto& v = ap::audio::MasterVolume::instance();
    check(v.percent() == 100 && !v.muted() && v.linear() == 1.0f, "starts at full volume");
    v.set(50, false);
    check(std::fabs(v.linear() - 0.25f) < 1e-6f, "50 % is about -12 dB (0.25)");
    v.set(0, false);
    check(v.linear() == 0.0f, "0 % is silent");
    v.set(80, true);
    check(v.linear() == 0.0f && v.percent() == 80, "mute silences but remembers the level");
    v.set(80, false);
    check(std::fabs(v.linear() - 0.64f) < 1e-6f, "unmute restores the level");
    v.set(250, false);
    check(v.percent() == 100, "values above 100 are clamped");
    v.set(-5, false);
    check(v.percent() == 0, "values below 0 are clamped");
    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
