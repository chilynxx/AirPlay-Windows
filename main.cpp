// WinAirPlay — Windows AirPlay 2 & Android Screen Mirroring Receiver entry point.
//
// Responsibilities of main():
//   1. Initialise Winsock (net::global_init) and Localization (i18n).
//   2. Build a DeviceContext (identifiers echoed in both mDNS TXT and /info).
//   3. Start the AirPlay RTSP server on TCP 7000 and Android Mirror Server on TCP 7001.
//   4. Start the native mDNS advertisement (Windows DnsService API) & USB Plug-and-Play Supervisor.
//   5. Block until window close or Ctrl-C, running the smart self-healing watchdog loop.

#include "airplay/hls_local_server.h"
#include "airplay/link_watchdog.h"
#include "airplay/live_settings.h"
#include "airplay/server.h"
#include "android/android_mirror.h"
#include "app_info.h"
#include "crypto/identity.h"
#include "i18n/localization.h"
#include "log.h"
#include "mdns/mdns_service.h"
#include "net/socket.h"
#include "platform/firewall.h"
#include "platform/single_instance.h"
#include "platform/thread_tuning.h"
#include "settings.h"
#include "usb/usb_supervisor.h"
#include "video/hls_player.h"
#include "video/video_renderer.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) { g_stop = true; }
#endif

ap::airplay::DeviceContext build_device_context(bool hls_playback) {
    ap::airplay::DeviceContext ctx;
    ctx.name     = ap::kAppName;
    ctx.deviceid = ap::net::primary_mac();
    ctx.model    = "AppleTV3,2";
    ctx.pi       = "b08f5a79-db29-4384-b456-a4784d9e6055";
    ctx.features = hls_playback ? "0x5A7FFEF7,0x400"   // video+HLS+HEVC
                                : "0x5A7FFEE6,0x400";  // HEVC mirror only
    ctx.srcvers  = "220.68";
    return ctx;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--configure-firewall") return ap::platform::configure_firewall();
    ap::platform::SingleInstance instance;
    bool help_only = false;
    for (int i = 1; i < argc; ++i)
        help_only |= std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h";
    if (!help_only) {
        const auto result = instance.acquire();
        if (result == ap::platform::SingleInstance::Result::Secondary) return 0;
        if (result == ap::platform::SingleInstance::Result::Error) {
            const std::wstring err_msg = ap::i18n::tr_w(ap::i18n::Str::SingleInstanceError);
            MessageBoxW(nullptr, err_msg.c_str(), ap::kAppNameW, MB_OK | MB_ICONERROR);
            return 1;
        }
    }
#endif

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--log" ||
            std::string(argv[i]) == "--verbose") {
            ap::set_log_enabled(true);
            break;
        }
    }

#if defined(_WIN32)
    const DWORD log_handle_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    const bool log_redirected = log_handle_type == FILE_TYPE_DISK || log_handle_type == FILE_TYPE_PIPE;
    if (ap::is_log_enabled() && !log_redirected) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
#endif

    ap::platform::disable_process_power_throttling();

    auto saved = ap::load_settings();
    if (saved.mirror_width == 0 || saved.mirror_height == 0) {
        saved.mirror_width = 2560;
        saved.mirror_height = 1440;
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            SDL_DisplayMode dm{};
            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 &&
                dm.w >= 320 && dm.h >= 240 &&
                dm.w <= 7680 && dm.h <= 4320) {
                saved.mirror_width = dm.w;
                saved.mirror_height = dm.h;
            }
            SDL_Quit();
        }
    }

    if (!ap::net::global_init()) {
        return 1;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    bool hls_playback = false;
    bool mirror_hwaccel = saved.mirror_hwaccel;
    int  mirror_w = saved.mirror_width;
    int  mirror_h = saved.mirror_height;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hls-proxy-playback" || arg == "--hls") {
            hls_playback = true;
        } else if (arg == "--mirror-hwaccel") {
            mirror_hwaccel = true;
        } else if ((arg == "--mirror-res" || arg == "-r") && i + 1 < argc) {
            const std::string val = argv[++i];
            const auto x = val.find_first_of("xX*");
            if (x != std::string::npos) {
                try {
                    const int w = std::stoi(val.substr(0, x));
                    const int h = std::stoi(val.substr(x + 1));
                    if (w >= 320 && h >= 240 &&
                        w <= 7680 && h <= 4320) {
                        mirror_w = w;
                        mirror_h = h;
                    } else {
                        LOG_WARN << "--mirror-res out of range (" << val
                                 << "), keeping default";
                    }
                } catch (...) {
                    LOG_WARN << "--mirror-res not parseable (" << val
                             << "), keeping default";
                }
            }
        } else if (arg == "--log" || arg == "--verbose") {
            // Handled in pre-init scan
        } else if (arg == "--help" || arg == "-h") {
            std::puts(
                "WinAirPlay [--log] [--hls-proxy-playback]\n"
                "           [--mirror-res WxH] [--mirror-hwaccel]\n"
                "  --log / --verbose     print init/runtime logs to stderr\n"
                "  --hls-proxy-playback  advertise HLS capability so iOS\n"
                "                        hands video URLs to our proxy\n"
                "  --mirror-res WxH      advertise the given display\n"
                "                        resolution (default: 2560x1440)\n"
                "  --mirror-hwaccel      decode mirror H.264/HEVC on the GPU\n"
                "                        (D3D11VA, GPU-resident presentation).");
            ap::net::global_shutdown();
            return 0;
        }
    }

    auto ctx = build_device_context(hls_playback);
    ctx.name           = saved.receiver_name;
    ctx.mirror_width   = mirror_w;
    ctx.mirror_height  = mirror_h;
    ctx.mirror_hwaccel = mirror_hwaccel;
    LOG_INFO << "Mirror display advertised: " << mirror_w << 'x' << mirror_h;
    LOG_INFO << "Mirror decoder default: "
             << (mirror_hwaccel
                   ? "GPU hwaccel (--mirror-hwaccel; D3D11VA resident surfaces)"
                   : "software libavcodec (toggle GPU in Settings panel)");

    ap::airplay::LiveSettings live_settings;
    live_settings.set_receiver_name(saved.receiver_name);
    live_settings.mirror_width.store(mirror_w);
    live_settings.mirror_height.store(mirror_h);
    live_settings.hevc_enabled.store(saved.hevc_enabled);
    live_settings.max_fps.store(saved.max_fps);
    live_settings.refresh_rate.store(saved.refresh_rate);
    live_settings.mirror_hwaccel.store(mirror_hwaccel);
    live_settings.vsync_enabled.store(saved.vsync_enabled);
    live_settings.fullscreen.store(saved.fullscreen);
    live_settings.chromeless.store(saved.chromeless);
    live_settings.video_rotation.store(saved.video_rotation);
    live_settings.auto_recover_seconds.store(saved.auto_recover_seconds);
    live_settings.set_ui_preferences(saved.ui);
    ctx.live = &live_settings;

    auto identity = ap::crypto::Identity::load_or_create("identity.key");
    if (!identity) {
        LOG_ERROR << "failed to initialise identity — aborting";
        ap::net::global_shutdown();
        return 1;
    }
    ctx.public_key = identity->public_key();
    ctx.identity   = identity.get();

    LOG_INFO << "=== WinAirPlay ===";
    LOG_INFO << "name="     << ctx.name;
    LOG_INFO << "deviceid=" << ctx.deviceid;
    const std::string local_ip   = ap::net::primary_ipv4();
    const std::string local_ipv6 = ap::net::primary_ipv6();
    LOG_INFO << "ip="       << local_ip;
    if (!local_ipv6.empty()) {
        LOG_INFO << "ipv6="  << local_ipv6;
    }
    LOG_INFO << "pk(Ed25519)=" << ctx.public_key.size() << " bytes";

    ap::video::VideoRenderer renderer;
    renderer.set_live_settings(&live_settings);
    if (!renderer.start(ap::kAppName)) {
        LOG_WARN << "VideoRenderer could not start — running headless";
    }
    const std::string display_ip =
        (local_ip.empty() || local_ip == "0.0.0.0") && !local_ipv6.empty()
            ? local_ipv6 : local_ip;
    renderer.set_idle_info(ctx.name, display_ip);
    ctx.renderer = &renderer;

    ap::video::HlsPlayer hls_player;
    if (hls_playback) ctx.hls_player = &hls_player;

    ap::airplay::Server server;
    if (!server.start(ctx, 7000)) {
        LOG_ERROR << "Failed to start AirPlay server";
        renderer.stop();
        ap::net::global_shutdown();
        return 1;
    }

    ap::android::AndroidMirrorServer android_server;
    android_server.attach_renderer(&renderer);
    android_server.set_live_settings(&live_settings);
    if (!android_server.start(7001)) {
        LOG_WARN << "AndroidMirrorServer could not bind TCP 7001";
    }

    std::atomic<bool> disconnect_requested{false};
    std::atomic<bool> reconnect_requested{false};
    renderer.set_disconnect_handler([&disconnect_requested]() {
        disconnect_requested.store(true);
    });
    renderer.set_reconnect_handler([&reconnect_requested]() {
        reconnect_requested.store(true);
    });
    renderer.set_android_connect_handler([&android_server](const std::string& target_ip) {
        android_server.connect_wireless_async(target_ip);
    });

    struct AudioRequest { ap::airplay::MediaCommand command; uint32_t position; std::string session_id; };
    std::mutex audio_request_mutex;
    std::optional<AudioRequest> audio_request;
    renderer.set_audio_control_handler([&](ap::airplay::MediaCommand command, uint32_t position) {
        ap::video::VideoRenderer::DeviceInfo device;
        renderer.active_device_snapshot(device);
        std::lock_guard<std::mutex> lock(audio_request_mutex);
        audio_request = AudioRequest{command, position, device.session_id};
    });

    ap::airplay::HlsLocalServer hls_server;
    if (hls_playback) {
        if (!hls_server.start(7100)) {
            LOG_WARN << "HLS local server could not bind 7100";
        }
        ctx.hls_local_port = hls_server.port();
    }

    ap::mdns::MdnsService mdns;
    if (!mdns.start(ctx, server.port())) {
        LOG_WARN << "mDNS not available — receiver won't auto-appear on iOS";
    }

    ap::usb::UsbSupervisor usb_supervisor;
    usb_supervisor.attach_renderer(&renderer);
    usb_supervisor.attach_android_server(&android_server);
    usb_supervisor.start();

    LOG_INFO << "Ready. Close the window or press Ctrl-C to exit.";
    auto last_saved_settings = ap::snapshot(live_settings);
    auto observed_settings   = last_saved_settings;
    auto settings_changed_at = std::chrono::steady_clock::now();
    ap::airplay::LinkWatchdog link_watchdog;

    while (!g_stop && !renderer.user_closed()) {
#if defined(_WIN32)
        if (instance.activation_requested()) renderer.request_show();
#endif
        const auto receiver_name = live_settings.receiver_name();
        if (receiver_name != ctx.name) {
            mdns.stop();
            ctx.name = receiver_name;
            renderer.set_idle_info(ctx.name, display_ip);
            if (!mdns.start(ctx, server.port()))
                LOG_WARN << "Could not advertise the new receiver name";
        }

        const bool link_active = renderer.mirror_link_active();
        const int64_t last_rx  = renderer.mirror_last_rx_ns();
        const int64_t now_ns   = ap::video::VideoRenderer::steady_now_ns();
        const int timeout_s    = live_settings.auto_recover_seconds.load();

        // Stage 1: Smart non-destructive probing & IDR keyframe flush during micro-stutters
        bool should_probe = false;
        const auto health = link_watchdog.evaluate(link_active, last_rx, now_ns, timeout_s, should_probe);
        if (health == ap::airplay::LinkHealth::MicroStutter ||
            health == ap::airplay::LinkHealth::Probing) {
            renderer.set_reconnecting_overlay(true);
            if (should_probe) {
                server.probe_and_recover_clients();
                android_server.request_keyframe_resync();
            }
        } else if (health == ap::airplay::LinkHealth::Healthy) {
            renderer.set_reconnecting_overlay(false);
        }

        // Stage 2: Full session reset & Bonjour restart if the link stays dead past timeout_s
        if (link_watchdog.poll(link_active, last_rx, now_ns, timeout_s)) {
            LOG_WARN << "Auto-recovery: no data from the device for more than "
                     << timeout_s
                     << " s — resetting the session and restarting Bonjour";
            server.disconnect_clients();
            android_server.disconnect_active();
            mdns.stop();
            if (!mdns.start(ctx, server.port()))
                LOG_WARN << "Auto-recovery: could not restart the Bonjour advertisement";
            renderer.note_auto_recovery();
            LOG_NOTICE << "Auto-recovery done: ready for a new connection";
        }

        // Manual Reconnect / Refresh Stream button on the Live Control Dock
        if (reconnect_requested.exchange(false)) {
            LOG_INFO << "Manual stream resync / reconnect requested from Control Dock";
            if (!server.probe_and_recover_clients() && !android_server.is_streaming()) {
                mdns.stop();
                mdns.start(ctx, server.port());
            } else {
                android_server.request_keyframe_resync();
            }
        }

        if (disconnect_requested.exchange(false)) {
            LOG_INFO << "Disconnect requested by toolbar";
            server.disconnect_clients();
            android_server.disconnect_active();
            LOG_INFO << "Disconnect completed: all client handlers drained";
        }

        std::optional<AudioRequest> request;
        {
            std::lock_guard<std::mutex> lock(audio_request_mutex);
            request.swap(audio_request);
        }
        if (request) {
            const bool success = server.control_audio(request->command, request->position, request->session_id);
            renderer.audio_command_finished(success);
        }
        ap::video::VideoRenderer::DeviceInfo current_device;
        renderer.active_device_snapshot(current_device);
        renderer.set_audio_control_available(server.audio_control_available(current_device.session_id));

        const auto current_settings = ap::snapshot(live_settings);
        const auto settings_now = std::chrono::steady_clock::now();
        if (!(current_settings == observed_settings)) {
            observed_settings = current_settings;
            settings_changed_at = settings_now;
        }
        if (!(current_settings == last_saved_settings) &&
            settings_now - settings_changed_at >= std::chrono::milliseconds(500)) {
            if (ap::save_settings(current_settings)) last_saved_settings = current_settings;
            settings_changed_at = settings_now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO << "Shutting down... (g_stop=" << g_stop.load()
             << " user_closed=" << renderer.user_closed() << ")";
    server.stop();
    android_server.stop();
    mdns.stop();
    usb_supervisor.stop();
    hls_player.stop();
    hls_server.stop();
    renderer.stop();
    ap::save_settings(ap::snapshot(live_settings));
    SDL_Quit();
    ap::net::global_shutdown();
    return 0;
}
