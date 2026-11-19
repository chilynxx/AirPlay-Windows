#pragma once

#include "airplay/mirror_listener.h"
#include "airplay/ntp_client.h"
#include "audio/audio_receiver.h"
#include "net/socket.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::airplay {

// Ports we allocate on our side. A 0 means "not used in this request".
struct StreamPorts {
    uint16_t server  = 0;   // RTP data (audio PCM/ALAC, video H.264)
    uint16_t control = 0;   // RTCP / events channel
    uint16_t timing  = 0;   // NTP-like timing
};

// One media stream in the AirPlay 2 model. iOS sends `streams[]` in the
// SETUP body; for each entry we bind a (data, control) UDP pair.
struct StreamChannel {
    int      type          = 0;  // 96 / 103 / 110
    uint16_t data_port     = 0;
    uint16_t control_port  = 0;
    socket_t data_sock     = INVALID_SOCK;
    socket_t control_sock  = INVALID_SOCK;
};

// Owns every UDP socket bound on behalf of one TCP session.
class StreamSession {
public:
    StreamSession();
    ~StreamSession();

    StreamSession(const StreamSession&)            = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    // Non-owning renderer hook. Must be set before setup_stream(110) if you
    // want decoded frames to hit a window.
    void set_renderer(ap::video::VideoRenderer* r) { renderer_ = r; }

    bool setup_legacy(const std::string& transport_header, StreamPorts& allocated);
    bool setup_session(uint16_t& event_port, uint16_t& timing_port);

    struct StreamOpts {
        std::vector<unsigned char> aes_key;
        std::vector<unsigned char> aes_iv;
        uint64_t                   stream_connection_id = 0;
        int                        ct          = 0;
        int                        sample_rate = 44100;
        bool                       mirror_hwaccel = true;
    };

    bool setup_stream(int type, uint16_t& data_port, uint16_t& control_port,
                      const StreamOpts& opts);

    bool start_ntp(const std::string& remote_ip, uint16_t remote_port);

    // Partial teardown for AirPlay 2 stream-level TEARDOWN requests.
    void stop_stream(int type);

    // Thread-safe: forwards to AudioReceiver if a type-96 stream is alive.
    void set_audio_volume_db(float db);
    void flush_audio(std::optional<uint32_t> boundary = {});
    void resume_audio();
    void set_audio_track(const std::string& track);

    // Request an immediate decoder keyframe resync on the active mirror listener.
    void request_keyframe_resync() {
        if (mirror_) mirror_->request_keyframe_resync();
    }

    void teardown();
    bool audio_only() const;

    const std::string& session_id() const { return session_id_; }

private:
    StreamPorts client_ports_{};
    socket_t    data_sock_   = INVALID_SOCK;
    socket_t    ctrl_sock_   = INVALID_SOCK;
    socket_t    timing_sock_ = INVALID_SOCK;

    socket_t    event_sock_     = INVALID_SOCK;
    socket_t    ap2_timing_sock = INVALID_SOCK;
    std::vector<StreamChannel>                    channels_;   // audio streams (UDP)
    std::unique_ptr<MirrorListener>               mirror_;     // video stream (TCP)
    std::unique_ptr<NtpClient>                    ntp_;
    std::unique_ptr<ap::audio::AudioReceiver>     audio_;      // type 96 UDP
    ap::video::VideoRenderer*                     renderer_{nullptr};
    bool audio_control_session_ = false;
    ap::audio::AudioReceiver::PauseState paused_audio_;
    std::string audio_track_;

    std::string session_id_;
};

} // namespace ap::airplay
