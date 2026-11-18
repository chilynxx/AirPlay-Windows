#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ap::audio {

// Remembers the volume the iPhone last announced (AirPlay "volume" parameter,
// in dB: 0 = full, about -30 = quietest, -144 = mute).
//
// Why this exists: the sound output is recreated every time the iPhone starts
// a new audio stream, for instance when you leave an app and come back. A new
// output starts at full volume, and the iPhone does not repeat its volume
// until the slider is touched, so the sound jumped to maximum until you
// pressed a volume button. The value is therefore kept for the whole process
// and applied whenever audio starts, and it is also what the receiver reports
// when the iPhone asks for the current volume.
class VolumeMemory {
public:
    // About half of the iPhone slider; used only before any volume was
    // announced, so a first stream never starts at full blast.
    static constexpr float kDefaultDb = -15.0f;

    static VolumeMemory& instance() {
        static VolumeMemory memory;
        return memory;
    }

    void set(float db) {
        // AirPlay's valid range is -144 dB (mute) through 0 dB (full).
        if (!std::isfinite(db)) db = kDefaultDb;
        db_.store(std::clamp(db, -144.0f, 0.0f), std::memory_order_release);
    }
    float get() const {
        const float value = db_.load(std::memory_order_acquire);
        return std::isfinite(value) ? value : kDefaultDb;
    }
    bool known() const { return std::isfinite(db_.load(std::memory_order_acquire)); }
    void forget() { db_.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_release); }

private:
    // A single atomic value avoids a two-atomic known/value race. NaN means
    // that the sender has not announced a volume in this process yet.
    std::atomic<float> db_{std::numeric_limits<float>::quiet_NaN()};
};

} // namespace ap::audio
