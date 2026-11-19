#pragma once
#include "app_info.h"
#include "audio/master_volume.h"
#include "i18n/localization.h"
#include "settings.h"

#include <atomic>
#include <mutex>
#include <string>

namespace ap::airplay {

// User-facing knobs that the UI mutates and that downstream code
// (the /info plist builder, the features-bits assembler) reads at
// AirPlay handshake time. Lives in a single instance owned by main()
// and is referenced by both the DeviceContext and the VideoRenderer
// so the overlay can change values that take effect on the next
// device /info request — practically: on the next reconnect.
//
// Numeric fields are atomic; the receiver name is protected by a mutex.
// The UI and RTSP threads can access these independently without racing.
struct LiveSettings {
    ap::UiPreferences ui_preferences() const {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        return ui_preferences_;
    }
    void set_ui_preferences(const ap::UiPreferences& value) {
        {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            ui_preferences_ = value;
        }
        ap::audio::MasterVolume::instance().set(value.master_volume, value.muted);
        ap::i18n::LocalizationManager::instance().set_language(
            static_cast<ap::i18n::Language>(value.language));
    }
    std::string receiver_name() const {
        std::lock_guard<std::mutex> lock(name_mutex_);
        return receiver_name_;
    }
    void set_receiver_name(const std::string& name) {
        std::lock_guard<std::mutex> lock(name_mutex_);
        receiver_name_ = name;
    }
    // Display box advertised in /info — see DeviceContext::mirror_*.
    std::atomic<int>  mirror_width{2560};
    std::atomic<int>  mirror_height{1440};

    // True -> advertise the "Screen Multi Codec" feature bit (high
    // word 0x400) so iOS is allowed to encode the mirror stream
    // with HEVC. False -> drop the bit and pin iOS to H.264, which
    // is more compatible with weaker decoders but uses ~50% more
    // bandwidth at the same visual quality.
    std::atomic<bool> hevc_enabled{true};

    // Frame-rate hints sent in the displays plist.
    std::atomic<int>  max_fps{60};
    std::atomic<int>  refresh_rate{60};

    // False: software decoding. True (default): D3D11VA decoding,
    // GPU color conversion and shared-texture presentation on the
    // same GPU adapter, without CPU readback for display.
    std::atomic<bool> mirror_hwaccel{true};

    // SDL_RENDERER_PRESENTVSYNC.
    std::atomic<bool> vsync_enabled{true};

    std::atomic<bool> fullscreen{false};
    std::atomic<bool> chromeless{false};
    // Display only: -1 follows the orientation encoded by the device; 0..3 adds
    // clockwise quarter turns to the received image. Recording is unchanged.
    std::atomic<int> video_rotation{-1};
    // Seconds of silence from the device before the session is reset (0 = off).
    std::atomic<int> auto_recover_seconds{8};

private:
    mutable std::mutex ui_mutex_;
    ap::UiPreferences ui_preferences_;
    mutable std::mutex name_mutex_;
    std::string receiver_name_ = kAppName;
};

} // namespace ap::airplay
