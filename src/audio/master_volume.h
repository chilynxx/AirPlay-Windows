#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ap::audio {

// The PC-side volume (slider and mute button in the window). It multiplies the
// level the iPhone announced, so pressing the iPhone's volume buttons and moving
// the slider in WWinAirPlay both change the loudness of the PC in real time.
// Process-wide and lock-free: the interface sets it, the audio thread reads it.
class MasterVolume {
public:
    static MasterVolume& instance() {
        static MasterVolume volume;
        return volume;
    }

    void set(int percent, bool muted) {
        const uint32_t value = static_cast<uint32_t>(std::clamp(percent, 0, 100));
        state_.store(value | (muted ? kMutedBit : 0u), std::memory_order_release);
    }
    int  percent() const { return static_cast<int>(state_.load(std::memory_order_acquire) & kPercentMask); }
    bool muted() const   { return (state_.load(std::memory_order_acquire) & kMutedBit) != 0; }

    // Factor applied to the samples. Squared so the slider feels even: 50 % is
    // about -12 dB instead of half the amplitude.
    float linear() const {
        const uint32_t value = state_.load(std::memory_order_acquire);
        if (value & kMutedBit) return 0.0f;
        const float p = static_cast<float>(value & kPercentMask) / 100.0f;
        return p * p;
    }

private:
    static constexpr uint32_t kPercentMask = 0xFFu;
    static constexpr uint32_t kMutedBit = 0x100u;
    std::atomic<uint32_t> state_{100u};
};

} // namespace ap::audio
