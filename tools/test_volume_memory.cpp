// The receiver must keep the volume the iPhone announced: the sound output is
// recreated whenever an app is left and re-entered, and the iPhone does not
// resend its volume until a volume button is pressed.

#include "audio/volume_memory.h"

#include <cmath>
#include <cstdio>
#include <limits>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main() {
    using ap::audio::VolumeMemory;
    auto& v = VolumeMemory::instance();

    check(!v.known(), "nothing announced yet");
    check(v.get() == VolumeMemory::kDefaultDb, "first stream starts at the default, not at full volume");
    check(VolumeMemory::kDefaultDb < -5.0f && VolumeMemory::kDefaultDb > -30.0f,
          "default is a moderate level");

    v.set(-12.5f);
    check(v.known() && std::fabs(v.get() + 12.5f) < 1e-6f, "announced volume is remembered");

    // A new audio stream (new output) asks the memory instead of using full volume.
    const float applied_to_new_stream = VolumeMemory::instance().get();
    check(std::fabs(applied_to_new_stream + 12.5f) < 1e-6f,
          "a new stream gets the last announced volume");

    v.set(-144.0f);
    check(v.get() <= -100.0f, "mute is remembered too");

    v.set(std::numeric_limits<float>::quiet_NaN());
    check(v.get() == VolumeMemory::kDefaultDb, "NaN volume is replaced with the safe default");
    v.set(std::numeric_limits<float>::infinity());
    check(v.get() == VolumeMemory::kDefaultDb, "infinite volume is replaced with the safe default");
    v.set(12.0f);
    check(v.get() == 0.0f, "positive dB values are clamped to full volume");
    v.set(-200.0f);
    check(v.get() == -144.0f, "values below the AirPlay mute floor are clamped");

    v.forget();
    check(v.get() == VolumeMemory::kDefaultDb, "forget() returns to the default");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
