#pragma once

#include "audio/aac_decoder.h"
#include "audio/audio_output.h"
#include "net/socket.h"
#include "video/h264_decoder.h"
#include "video/video_renderer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ap::airplay { struct LiveSettings; }

namespace ap::android {

// Modular Android Screen & Audio Mirroring Subsystem (Wireless TCP/IP + Wired USB ADB).
//
// Parallel to ap::airplay::Server + MirrorListener:
//   1. Listens on TCP port 7001 for incoming Android H.264/HEVC video & PCM/AAC audio
//      streams (supports both framed WinAirPlay "WAPK" packets and raw Annex-B H.264
//      streams from Android's `screenrecord --output-format=h264 -`).
//   2. Integrates a native ADB Smart-Socket client (127.0.0.1:5037) to initiate
//      Wireless ADB connections (`connect_wireless("192.168.1.x:5555")`) and pull
//      zero-copy H.264 streams over USB or Wi-Fi without external scripts.
//   3. Feeds demuxed H.264/HEVC NAL units directly into our D3D11VA hardware-accelerated
//      `H264Decoder` -> `VideoRenderer::push_avframe()` -> `VideoRecorder`, and routes
//      decoded PCM/AAC audio into `SdlAudioOutput` (WASAPI).
class AndroidMirrorServer {
public:
    AndroidMirrorServer();
    ~AndroidMirrorServer();

    AndroidMirrorServer(const AndroidMirrorServer&)            = delete;
    AndroidMirrorServer& operator=(const AndroidMirrorServer&) = delete;

    void attach_renderer(ap::video::VideoRenderer* renderer) { renderer_ = renderer; }
    void set_live_settings(ap::airplay::LiveSettings* live)  { live_ = live; }

    // Start the TCP 7001 listener thread for incoming Android streams.
    bool start(uint16_t port = 7001);
    void stop();

    uint16_t port() const noexcept { return listen_port_.load(std::memory_order_relaxed); }
    bool is_streaming() const noexcept { return active_streaming_.load(std::memory_order_acquire); }

    // Initiate an asynchronous Wireless connection to an Android device at `ip_or_host_port`
    // (e.g. "192.168.1.50" or "192.168.1.50:5555"). Connects via ADB TCP/IP (5555) or
    // direct socket (7001) and starts streaming automatically.
    bool connect_wireless_async(const std::string& ip_or_host_port);

    // Initiate an asynchronous USB mirroring stream for an authorized Android device
    // discovered by UsbSupervisor via ADB (`serial`).
    bool connect_usb_adb_async(const std::string& serial, const std::string& model_name);

    // Drop any currently active Android mirror session.
    void disconnect_active();

    // Flush the H.264/HEVC reference frames and resync on the next IDR keyframe.
    void request_keyframe_resync() {
        resync_requested_.store(true, std::memory_order_release);
    }

    std::string last_status() const;

private:
    void accept_loop();
    void stream_worker(socket_t sock, std::string peer_name, std::string model_name, bool is_usb);
    void adb_connect_worker(std::string target, std::string model_name, bool is_usb);

    socket_t                   listen_sock_{INVALID_SOCK};
    std::atomic<uint16_t>      listen_port_{7001};
    std::atomic<bool>          running_{false};
    std::atomic<bool>          active_streaming_{false};
    std::atomic<bool>          resync_requested_{false};

    std::thread                accept_thread_;
    std::mutex                 worker_mtx_;
    std::thread                stream_thread_;
    std::thread                adb_thread_;
    socket_t                   active_sock_{INVALID_SOCK};

    mutable std::mutex         status_mtx_;
    std::string                last_status_;

    ap::video::VideoRenderer*  renderer_{nullptr};
    ap::airplay::LiveSettings* live_{nullptr};
};

} // namespace ap::android
