#pragma once

#include "airplay/media_control.h"
#include "app_info.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
struct AVFrame;
struct AVBufferRef;
}

namespace ap::airplay { struct LiveSettings; }

namespace ap::video {

class VideoRecorder;

// RAII handle around an FFmpeg refcounted AVFrame so the
// renderer can own decoder output without copying any pixel
// data — av_frame_clone shares the underlying buffer; the
// destructor releases it back to libavcodec's pool. Move-only.
struct DecodedFrame {
    AVFrame* frame = nullptr;
    DecodedFrame() = default;
    explicit DecodedFrame(AVFrame* f) : frame(f) {}
    ~DecodedFrame();

    DecodedFrame(const DecodedFrame&)            = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
    DecodedFrame(DecodedFrame&& o) noexcept;
    DecodedFrame& operator=(DecodedFrame&& o) noexcept;
};

// SDL2-backed real-time renderer for the decoded iOS AirPlay and Android mirror streams.
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&)            = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    // Initialise SDL, create the window at a default size, start the
    // rendering thread. Returns false on SDL failure.
    bool start(const std::string& title = ap::kAppName);

    // Tell the thread to exit, join it, tear down SDL.
    void stop();

    // Process window activation on its owning render thread.
    void request_show() { show_requested_.store(true); }

    // Producer entry points (I420, NV12, and zero-copy refcounted AVFrame).
    void push_frame(const uint8_t* y, int y_stride,
                    const uint8_t* u, int u_stride,
                    const uint8_t* v, int v_stride,
                    int width, int height,
                    int64_t origin_ns = 0);

    void push_frame_nv12(const uint8_t* y,  int y_stride,
                         const uint8_t* uv, int uv_stride,
                         int width, int height,
                         int64_t origin_ns = 0);

    void push_avframe(const AVFrame* src, int64_t origin_ns = 0);

    // ---- link supervision & smart auto-reconnect (used by main()) ---------
    static int64_t steady_now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void begin_mirror_link() {
        reconnecting_overlay_.store(false, std::memory_order_relaxed);
        unsigned count = mirror_link_count_.load(std::memory_order_acquire);
        for (;;) {
            if (count == 0)
                mirror_last_rx_ns_.store(steady_now_ns(), std::memory_order_relaxed);
            if (mirror_link_count_.compare_exchange_weak(count, count + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                break;
        }
    }
    void end_mirror_link() {
        reconnecting_overlay_.store(false, std::memory_order_relaxed);
        unsigned count = mirror_link_count_.load(std::memory_order_acquire);
        while (count != 0 &&
               !mirror_link_count_.compare_exchange_weak(count, count - 1,
                   std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
    void note_mirror_data() {
        mirror_last_rx_ns_.store(steady_now_ns(), std::memory_order_relaxed);
        reconnecting_overlay_.store(false, std::memory_order_relaxed);
    }
    bool    mirror_link_active() const { return mirror_link_count_.load(std::memory_order_acquire) != 0; }
    int64_t mirror_last_rx_ns()  const { return mirror_last_rx_ns_.load(std::memory_order_relaxed); }

    // Set true by main() when LinkWatchdog detects a Wi-Fi micro-stutter (>1.25s silence)
    // and is actively probing via RTSP GET_PARAMETER + IDR resync before full timeout.
    void set_reconnecting_overlay(bool active) noexcept {
        reconnecting_overlay_.store(active, std::memory_order_relaxed);
    }
    bool is_reconnecting_overlay() const noexcept {
        return reconnecting_overlay_.load(std::memory_order_relaxed);
    }

    // Recorded when the session was reset automatically; the UI shows a notice.
    void note_auto_recovery() {
        reconnecting_overlay_.store(false, std::memory_order_relaxed);
        auto_recovery_ns_.store(steady_now_ns(), std::memory_order_relaxed);
        auto_recovery_count_.fetch_add(1, std::memory_order_relaxed);
    }
    int64_t  auto_recovery_ns()    const { return auto_recovery_ns_.load(std::memory_order_relaxed); }
    uint64_t auto_recovery_count() const { return auto_recovery_count_.load(std::memory_order_relaxed); }

    // ---- USB Plug-and-Play status (updated by UsbSupervisor) --------------
    void set_usb_status(int state, const std::string& device_name, const std::string& identifier) {
        std::lock_guard<std::mutex> lock(usb_mtx_);
        usb_state_       = state;
        usb_device_name_ = device_name;
        usb_identifier_  = identifier;
    }

    // ---- feedback for the decode side -------------------------------------
    int  gpu_convert_failures() const {
        return gpu_convert_failures_.load(std::memory_order_relaxed);
    }
    void reset_gpu_convert_failures() {
        gpu_convert_failures_.store(0, std::memory_order_relaxed);
    }
    uint64_t frames_presented() const { return frames_presented_.load(std::memory_order_relaxed); }
    uint64_t frames_replaced()  const { return frames_replaced_.load(std::memory_order_relaxed); }

    // Caller owns the returned FFmpeg ref. Null when D3D11 is unavailable.
    AVBufferRef* gpu_device_ref();

    // Decoded RAOP/Android PCM, forwarded to the active MP4 recorder.
    void push_audio_pcm(const int16_t* samples, int sample_count,
                        int sample_rate, int channels);

    void record_payload_bytes(std::size_t n);

    void push_cover_art(const uint8_t* jpeg, std::size_t size);

    void push_metadata(const std::string& title,
                       const std::string& artist,
                       const std::string& album);

    void push_progress(uint32_t elapsed_ms, uint32_t total_ms);

    void push_playback_rate(float rate);

    void clear_session();

    void note_flush();
    bool in_flush_grace() const;

    void set_idle_info(const std::string& name, const std::string& ip);

    struct DeviceInfo {
        std::string peer_ip;
        std::string session_id;
        std::string kind;        // "Mirror", "HLS", "Audio", "USB", "Android"
        std::string name;        // Device display name
        std::string model;       // Device model
    };
    void set_active_device(DeviceInfo info);
    void clear_active_device();
    bool clear_session_if_active(const std::string& session_id);
    bool active_device_snapshot(DeviceInfo& out) const;

    void set_live_settings(ap::airplay::LiveSettings* live);

    void set_audio_control_handler(std::function<void(ap::airplay::MediaCommand, uint32_t)> handler);
    void set_audio_control_available(bool available) { audio_control_available_.store(available); }
    void audio_command_finished(bool success) {
        audio_command_failed_.store(!success);
        audio_command_pending_.store(false);
    }

    void set_disconnect_handler(std::function<void()> h);
    void set_reconnect_handler(std::function<void()> h) { reconnect_handler_ = std::move(h); }
    void set_android_connect_handler(std::function<void(const std::string&)> h) {
        android_connect_handler_ = std::move(h);
    }

    bool user_closed() const { return closed_.load(); }

private:
    void run(const std::string& title);

    std::atomic<bool> running_{false};
    std::atomic<bool> show_requested_{false};
    std::atomic<bool> closed_{false};
    std::thread       thread_;

    std::mutex                 mtx_;
    std::condition_variable    cv_;
    AVBufferRef*               gpu_device_ = nullptr;
    bool                       gpu_ready_ = false;
    std::vector<unsigned char> y_buf_;
    std::vector<unsigned char> u_buf_;
    std::vector<unsigned char> v_buf_;
    std::vector<unsigned char> uv_buf_;     // NV12 only
    int                        frame_w_{0};
    int                        frame_h_{0};
    bool                       has_frame_{false};
    bool                       frame_is_nv12_{false};
    int64_t                    frame_origin_ns_{0};
    DecodedFrame               slot_avframe_;
    bool                       has_avframe_{false};

    std::mutex                 cover_mtx_;
    std::vector<unsigned char> cover_jpeg_;
    bool                       cover_dirty_{false};

    std::mutex                 meta_mtx_;
    std::string                meta_title_;
    std::string                meta_artist_;
    std::string                meta_album_;
    bool                       meta_dirty_{false};

    std::string                idle_name_;
    std::string                idle_ip_;
    bool                       idle_dirty_{true};

    std::atomic<uint32_t>      progress_elapsed_ms_{0};
    std::atomic<uint32_t>      progress_total_ms_{0};
    std::atomic<int64_t>       progress_pushed_at_ns_{0};
    std::atomic<bool>          playing_{true};
    std::atomic<bool>          clear_cover_requested_{false};
    std::atomic<int64_t>       flush_grace_until_ns_{0};

    mutable std::mutex         device_mtx_;
    DeviceInfo                 device_;
    bool                       has_device_{false};

    mutable std::mutex         usb_mtx_;
    int                        usb_state_{0};
    std::string                usb_device_name_;
    std::string                usb_identifier_;

    std::atomic<float>         video_fps_ema_{0.0f};
    std::atomic<int64_t>       last_push_ns_{0};
    std::atomic<int>           video_w_seen_{0};
    std::atomic<int>           video_h_seen_{0};
    std::atomic<uint64_t>      frames_total_{0};
    std::atomic<int>           gpu_convert_failures_{0};
    std::atomic<unsigned>      mirror_link_count_{0};
    std::atomic<int64_t>       mirror_last_rx_ns_{0};
    std::atomic<bool>          reconnecting_overlay_{false};
    std::atomic<int64_t>       auto_recovery_ns_{0};
    std::atomic<uint64_t>      auto_recovery_count_{0};
    std::atomic<uint64_t>      frames_presented_{0};
    std::atomic<uint64_t>      frames_replaced_{0};

    std::atomic<uint64_t>      payload_bytes_total_{0};
    std::atomic<float>         payload_mbps_ema_{0.0f};
    std::atomic<int64_t>       last_payload_ns_{0};
    std::atomic<float>         pipeline_latency_ms_ema_{0.0f};

    ap::airplay::LiveSettings* live_settings_ = nullptr;

    std::function<void()>      disconnect_handler_;
    std::function<void()>      reconnect_handler_;
    std::function<void(const std::string&)> android_connect_handler_;
    std::function<void(ap::airplay::MediaCommand, uint32_t)> audio_control_handler_;
    std::atomic<bool> audio_control_available_{false};
    std::atomic<bool> audio_command_pending_{false};
    std::atomic<bool> audio_command_failed_{false};

    std::unique_ptr<VideoRecorder> recorder_;
};

} // namespace ap::video
