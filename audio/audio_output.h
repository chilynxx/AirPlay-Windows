#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ap::audio {

// Push-mode PCM sink backed by SDL2 Audio (WASAPI on Windows, ALSA /
// PulseAudio on Linux). The producer calls push() with interleaved int16
// samples; SDL handles pacing and delivers them to the OS mixer.
//
// We don't use SDL's callback pull mode because the producer cadence
// (decoder output) is already naturally paced by the network, and the
// queue lets us absorb short bursts cleanly without locking the
// decoder thread on audio hardware.
class SdlAudioOutput {
public:
    SdlAudioOutput();
    ~SdlAudioOutput();

    SdlAudioOutput(const SdlAudioOutput&)            = delete;
    SdlAudioOutput& operator=(const SdlAudioOutput&) = delete;

    bool start(int sample_rate, int channels);
    void stop();

    // Enqueue interleaved int16 samples. `count` is the total number of
    // int16 samples (NOT frames) — for stereo, that's 2 * frame_count.
    // Non-blocking. The current volume gain is applied in-place to an
    // internal scratch buffer before SDL_QueueAudio.
    void push(const int16_t* samples, int count);

    // Set playback gain from a dB value in AirPlay's convention:
    //   0 dB   = full volume
    //   -144 dB or below = mute
    // Internally converted to a linear multiplier. Thread-safe.
    void set_volume_db(float db);

    // Bytes currently queued (not yet played). Useful for back-pressure.
    uint32_t queued_bytes() const;
    int64_t pending_ns() const;
    void clear();

    // Device watchdog (call from the single thread that calls push()).
    // Returns true when PCM is queued but the device has consumed none of it
    // for `timeout` — typical for a Bluetooth/HDMI/USB output that went to
    // sleep or an endpoint that was switched away. The caller should then
    // call reopen().
    bool stalled(std::chrono::milliseconds timeout);

    // False after stop(), or after a reopen() that could not get a device.
    bool is_open() const { return device_.load() != 0; }

    // Close and reopen the device with the same format. Gain is preserved.
    // Not thread-safe against push(); call from the pushing thread.
    bool reopen();

private:
    std::atomic<unsigned int> device_{0};   // SDL_AudioDeviceID is uint32_t
    std::atomic<uint64_t>     pushed_bytes_{0};
    uint64_t                  last_consumed_{0};
    std::chrono::steady_clock::time_point last_progress_{std::chrono::steady_clock::now()};
    int                channels_{0};
    int                sample_rate_{0};
    int                buffer_frames_{0};
    std::atomic<float> gain_{1.0f};  // linear multiplier, 0.0 = muted
};

} // namespace ap::audio
