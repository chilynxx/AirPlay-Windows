#pragma once

#include "crypto/mirror_decrypt.h"
#include "net/socket.h"
#include "video/h264_decoder.h"
#include "video/video_renderer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ap::airplay {

// TCP listener + frame parser for the AirPlay 2 mirror stream (type 110).
//
// Ported from UxPlay's raop_rtp_mirror.c (GPL-3.0). The mirror stream is a
// TCP byte stream in which each frame is a 128-byte little-endian header
// followed by a variable-length payload:
//
//   [0..3]   payload_size  — LE uint32
//   [4..5]   payload_type  — (see table below)
//   [6..7]   payload_option
//   [8..15]  timestamp     — LE uint64 (NTP-like, fraction in low 32 bits)
//   [16..127] reserved / image-size data for SPS/PPS packets
//   [128..]  payload (NAL units: encrypted H.264 or clear SPS/PPS)
//
// payload_type values (first 2 bytes at offset 4..5):
//   0x00 0x00 → AES-CTR encrypted VCL NAL type 1 (non-IDR video frame)
//   0x00 0x10 → AES-CTR encrypted VCL NAL type 5 (IDR keyframe)
//   0x01 0x00 → CLEAR SPS + PPS (types 7 + 8), sent at init and format changes
//   0x02 0x00 → old-protocol heartbeat, no payload
//   0x05 0x00 → "streaming report" (once per second, no video)
//
// For now we parse, count, and log. Decryption + H.264 decode is the next
// milestone (needs playfair for the AES key).
class MirrorListener {
public:
    MirrorListener();
    ~MirrorListener();

    MirrorListener(const MirrorListener&)            = delete;
    MirrorListener& operator=(const MirrorListener&) = delete;

    bool start(uint16_t& port);
    void stop();

    // Enable in-place AES-CTR decryption of VIDEO_IDR / VIDEO_NON_IDR frame
    // payloads before they are logged. Call BEFORE start(). Pass the audio
    // AES key (from fairplay_decrypt) and the streamConnectionID from the
    // per-stream SETUP entry. `hwaccel` requests D3D11VA on Windows for
    // the H.264/HEVC decoder. Attach the renderer before this call so its
    // D3D11 device is available when the decoder is initialized.
    bool enable_decrypt(const std::vector<unsigned char>& aes_key_audio,
                        uint64_t stream_connection_id,
                        bool hwaccel = false);

    // Attach an external renderer — decoded frames (YUV420P) are pushed to
    // it on the reader thread. The listener does not own the renderer.
    void attach_renderer(ap::video::VideoRenderer* renderer) { renderer_ = renderer; }

    // How long the sender may stay completely silent before the stream is
    // closed (default 30 s). Adjustable mainly for tests.
    void set_idle_limit(std::chrono::milliseconds limit) { idle_limit_ms_ = limit.count(); }

    // Interval of the "health:" log line (default 10 s). Adjustable for tests.
    void set_health_period(std::chrono::milliseconds period) { health_period_ms_ = period.count(); }

    // How long the stream may produce no new picture (while the sender is still
    // connected) before it is reported in the log (default 20 s). Adjustable for tests.
    void set_stall_warn(std::chrono::milliseconds first) { stall_warn_ms_ = first.count(); }

    // Pictures decoded so far in this listener's lifetime (diagnostics/tests).
    uint64_t decoded_frames() const { return decoded_frames_.load(std::memory_order_relaxed); }

private:
    void accept_loop();
    void reader_loop(socket_t client);

    socket_t          listen_sock_{INVALID_SOCK};
    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::mutex        client_mtx_;
    socket_t          client_sock_{INVALID_SOCK};

    std::unique_ptr<ap::crypto::MirrorDecrypt> decrypt_;
    std::unique_ptr<ap::video::H264Decoder>    decoder_;
    ap::video::VideoRenderer*                  renderer_{nullptr};
    std::atomic<int64_t>                       idle_limit_ms_{30'000};
    std::atomic<int64_t>                       health_period_ms_{10'000};
    std::atomic<int64_t>                       stall_warn_ms_{20'000};
    std::atomic<uint64_t>                      decoded_frames_{0};
};

} // namespace ap::airplay
