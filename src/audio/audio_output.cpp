#include "audio/audio_output.h"
#include "audio/master_volume.h"
#include "audio/volume_memory.h"
#include "log.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ap::audio {

SdlAudioOutput::SdlAudioOutput()  = default;
SdlAudioOutput::~SdlAudioOutput() { stop(); }

bool SdlAudioOutput::start(int sample_rate, int channels) {
    // The subsystem is deliberately never shut down (see stop()), so only
    // initialise it when it is not already up. reopen() would otherwise
    // keep raising its reference count.
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_ERROR << "SDL_InitSubSystem(AUDIO) failed: " << SDL_GetError();
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = sample_rate;
    want.format   = AUDIO_S16SYS;     // interleaved int16, host byte order
    want.channels = static_cast<Uint8>(channels);
    want.samples  = 1024;             // buffer granularity (~23 ms @ 44.1 kHz)
    want.callback = nullptr;          // push mode via SDL_QueueAudio

    SDL_AudioSpec got{};
    // PCM is always supplied at the decoder's frequency/channel count.
    // SDL performs any hardware conversion instead of changing this contract.
    const SDL_AudioDeviceID opened = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (opened == 0) {
        LOG_ERROR << "SDL_OpenAudioDevice failed: " << SDL_GetError();
        return false;
    }
    device_ = opened;

    sample_rate_ = got.freq;
    channels_    = got.channels;
    buffer_frames_ = got.samples;
    pushed_bytes_  = 0;
    last_consumed_ = 0;
    last_progress_ = std::chrono::steady_clock::now();
    SDL_PauseAudioDevice(device_, 0);
    LOG_INFO << "SdlAudioOutput playing at " << sample_rate_ << " Hz x "
             << channels_ << "ch (format int16)";
    return true;
}

void SdlAudioOutput::stop() {
    const SDL_AudioDeviceID dev = device_.exchange(0);
    if (dev) {
        SDL_PauseAudioDevice(dev, 1);
        SDL_ClearQueuedAudio(dev);
        SDL_CloseAudioDevice(dev);
        // Deliberately NOT calling SDL_QuitSubSystem(SDL_INIT_AUDIO):
        // stop() runs on the RTSP server thread during session
        // teardown, and on Windows shutting down the SDL audio
        // subsystem cross-thread can knock out the video event pump
        // (spontaneous SDL_QUIT / window close). SDL_Quit() in the
        // renderer's shutdown path will clean up the subsystem when
        // the app actually exits.
    }
}

void SdlAudioOutput::push(const int16_t* samples, int count) {
    if (!device_ || !samples || count <= 0) return;

    const float gain = gain_.load(std::memory_order_relaxed) * ap::audio::MasterVolume::instance().linear();

    // Fast path: unity gain → send as-is.
    if (gain >= 0.9999f && gain <= 1.0001f) {
        const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
        if (SDL_QueueAudio(device_, samples, bytes) != 0) {
            LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
        } else {
            pushed_bytes_ += bytes;
        }
        return;
    }

    // Apply gain into a scratch buffer, saturate to int16 range.
    static thread_local std::vector<int16_t> scratch;
    scratch.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        float s = static_cast<float>(samples[i]) * gain;
        if (s >  32767.f) s =  32767.f;
        if (s < -32768.f) s = -32768.f;
        scratch[i] = static_cast<int16_t>(s);
    }
    const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
    if (SDL_QueueAudio(device_, scratch.data(), bytes) != 0) {
        LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
    } else {
        pushed_bytes_ += bytes;
    }
}

void SdlAudioOutput::set_volume_db(float db) {
    // AirPlay: 0 dB = full, -144 dB = mute. Reject NaN/Inf and clamp values.
    if (!std::isfinite(db)) db = VolumeMemory::kDefaultDb;
    db = std::clamp(db, -144.0f, 0.0f);
    const float lin = (db <= -100.f) ? 0.f : std::pow(10.f, db / 20.f);
    gain_.store(lin, std::memory_order_relaxed);
    LOG_INFO << "audio volume: " << db << " dB (gain=" << lin << ')';
}

uint32_t SdlAudioOutput::queued_bytes() const {
    return device_ ? SDL_GetQueuedAudioSize(device_) : 0u;
}

int64_t SdlAudioOutput::pending_ns() const {
    if (!device_ || sample_rate_ <= 0 || channels_ <= 0) return 0;
    const int64_t frames = queued_bytes() / (channels_ * sizeof(int16_t));
    // SDL does not expose the hardware presentation clock. Include one
    // negotiated device buffer as an estimate, rather than a fixed 25 ms.
    return (frames + buffer_frames_) * 1'000'000'000LL / sample_rate_;
}

void SdlAudioOutput::clear() {
    if (device_) SDL_ClearQueuedAudio(device_);
}

bool SdlAudioOutput::stalled(std::chrono::milliseconds timeout) {
    if (!device_) return false;
    const uint64_t pushed   = pushed_bytes_.load(std::memory_order_relaxed);
    const uint32_t queued   = SDL_GetQueuedAudioSize(device_);
    const uint64_t consumed = pushed - queued;   // also moves on clear()
    const auto now = std::chrono::steady_clock::now();
    if (queued == 0 || consumed != last_consumed_) {
        last_consumed_ = consumed;
        last_progress_ = now;
        return false;
    }
    return now - last_progress_ >= timeout;
}

bool SdlAudioOutput::reopen() {
    const int rate = sample_rate_;
    const int ch   = channels_;
    if (rate <= 0 || ch <= 0) return false;
    LOG_WARN << "SdlAudioOutput: reopening the sound device";
    stop();
    return start(rate, ch);
}

} // namespace ap::audio
