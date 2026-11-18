#pragma once

#include "app_info.h"

#include <filesystem>
#include <string>
#include <tuple>

namespace ap::airplay { struct LiveSettings; }

namespace ap {

struct UiPreferences {
    bool diagnostics = false;
    bool window_placement_valid = false;
    int window_x = 0, window_y = 0;
    int window_width = 1220, window_height = 800;
    bool window_maximized = false;
    std::string recording_directory;
    int recording_codec = 0;   // H.264 / H.265
    int recording_encoder = 0; // Auto / GPU / CPU
    int recording_bitrate = 8;
    // Picture shown behind the idle screens (empty = none) and how much it is darkened.
    std::string wallpaper_path;
    int wallpaper_dim = 45;   // percent, 0..90
    int accent = 0;           // colour preset, see receiver_ui.h (0..3)
    bool device_frame = false; // phone bezel around the mirrored picture
    int master_volume = 100;  // PC-side volume, percent
    bool muted = false;

    bool operator==(const UiPreferences& other) const {
        return std::tie(diagnostics, window_placement_valid, window_x, window_y,
                        window_width, window_height, window_maximized, recording_directory,
                        recording_codec, recording_encoder, recording_bitrate,
                        wallpaper_path, wallpaper_dim, accent, device_frame, master_volume, muted) ==
               std::tie(other.diagnostics, other.window_placement_valid, other.window_x, other.window_y,
                        other.window_width, other.window_height, other.window_maximized, other.recording_directory,
                        other.recording_codec, other.recording_encoder, other.recording_bitrate,
                        other.wallpaper_path, other.wallpaper_dim, other.accent,
                        other.device_frame, other.master_volume, other.muted);
    }
};

struct PersistentSettings {
    std::string receiver_name = kAppName;
    int  mirror_width   = 0;
    int  mirror_height  = 0;
    bool hevc_enabled   = true;
    int  max_fps        = 60;
    int  refresh_rate   = 60;
    bool mirror_hwaccel = true;
    bool vsync_enabled  = true;
    bool fullscreen     = false;
    bool chromeless     = false;
    int video_rotation  = -1; // -1: follow stream; 0..3: clockwise quarter turns
    // Seconds of complete silence from the iPhone after which the session is
    // reset and Bonjour restarted so it can reconnect at once. 0 = off.
    int auto_recover_seconds = 8;
    UiPreferences ui;

    bool operator==(const PersistentSettings& other) const {
        return std::tie(receiver_name, mirror_width, mirror_height, hevc_enabled, max_fps,
                        refresh_rate, mirror_hwaccel, vsync_enabled, fullscreen, chromeless, video_rotation,
                        auto_recover_seconds, ui) ==
               std::tie(other.receiver_name, other.mirror_width, other.mirror_height, other.hevc_enabled, other.max_fps,
                        other.refresh_rate, other.mirror_hwaccel, other.vsync_enabled, other.fullscreen, other.chromeless, other.video_rotation,
                        other.auto_recover_seconds, other.ui);
    }
};

std::filesystem::path settings_path();
PersistentSettings  load_settings(const std::filesystem::path& path = settings_path());
bool                save_settings(const PersistentSettings& s, const std::filesystem::path& path = settings_path());
PersistentSettings  snapshot(const ap::airplay::LiveSettings& live);

} // namespace ap
