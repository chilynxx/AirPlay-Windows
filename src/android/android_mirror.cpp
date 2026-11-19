#include "android/android_mirror.h"
#include "airplay/h264_nal.h"
#include "airplay/h264_sps.h"
#include "airplay/live_settings.h"
#include "log.h"
#include "platform/thread_tuning.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

extern "C" {
#include <libavutil/buffer.h>
}

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <errno.h>
#endif

namespace ap::android {
namespace {

constexpr int kRecvTimeoutMs = 200;
constexpr uint32_t kMaxFrameBytes = 4'000'000;

// Framed WinAirPlay Android packet header ("WAPK" magic):
//   [0..3]   "WAPK"
//   [4]      packet_type: 1 = H.264 Annex-B/AVCC, 2 = HEVC, 3 = PCM16 Stereo, 4 = AAC-LC, 5 = Rotation
//   [5]      flags: bit 0 = IDR keyframe, bits 1..2 = clockwise rotation (0..3)
//   [6..7]   sample_rate_div100 (for audio, e.g. 480 -> 48000 Hz, 441 -> 44100 Hz)
//   [8..11]  payload_size (LE uint32)
//   [12..19] timestamp_ns (LE uint64)
constexpr int kWapkHeaderSize = 20;

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void set_socket_timeout(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool socket_timed_out() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Build a synthetic ISO 14496-15 avcC configuration record from raw SPS + PPS
// NAL bytes (without start codes) so H264Decoder::set_parameter_sets_from_avcc()
// can configure and cache them identically to AirPlay 2.
std::vector<uint8_t> build_avcc_from_sps_pps(const std::vector<uint8_t>& sps,
                                             const std::vector<uint8_t>& pps) {
    if (sps.size() < 4 || pps.empty()) return {};
    std::vector<uint8_t> avcc;
    avcc.reserve(11 + sps.size() + pps.size());
    avcc.push_back(0x01);          // configurationVersion = 1
    avcc.push_back(sps[1]);        // AVCProfileIndication
    avcc.push_back(sps[2]);        // profile_compatibility
    avcc.push_back(sps[3]);        // AVCLevelIndication
    avcc.push_back(0xFF);          // 6 bits reserved (1) + lengthSizeMinusOne (3 -> 4 bytes)
    avcc.push_back(0xE1);          // 3 bits reserved (1) + numOfSequenceParameterSets (1)
    avcc.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(0x01);          // numOfPictureParameterSets = 1
    avcc.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return avcc;
}

// Find the next Annex-B start code (00 00 01 or 00 00 00 01) starting at offset `from`.
// Returns the index of the first 0x00 of the start code, and sets `sc_len` to 3 or 4.
std::size_t find_annexb_start_code(const uint8_t* data, std::size_t size,
                                   std::size_t from, std::size_t& sc_len) {
    sc_len = 0;
    if (size < 3 || from + 3 > size) return std::string::npos;
    for (std::size_t i = from; i + 3 <= size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                if (i > from && data[i - 1] == 0x00) {
                    sc_len = 4;
                    return i - 1;
                }
                sc_len = 3;
                return i;
            }
            if (i + 4 <= size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                sc_len = 4;
                return i;
            }
        }
    }
    return std::string::npos;
}

// Connect to local ADB server (127.0.0.1:5037).
socket_t connect_tcp_ipv4(const std::string& host, uint16_t port, int timeout_ms = 1500) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return INVALID_SOCK;
    set_socket_timeout(s, timeout_ms);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ap::net::close_socket(s);
        return INVALID_SOCK;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ap::net::close_socket(s);
        return INVALID_SOCK;
    }
    return s;
}

// Send an ADB smart-socket command ("000Chost:version") and verify "OKAY".
bool adb_command(socket_t s, const std::string& cmd, std::string* response_payload = nullptr) {
    char prefix[5];
    std::snprintf(prefix, sizeof(prefix), "%04X", static_cast<unsigned>(cmd.size()));
    std::string wire = std::string(prefix, 4) + cmd;
    if (ap::net::send_all(s, wire.data(), static_cast<int>(wire.size())) < 0) {
        return false;
    }
    char status[4] = {};
    int got = 0;
    while (got < 4) {
        int n = ::recv(s, status + got, 4 - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    if (std::memcmp(status, "OKAY", 4) != 0) {
        return false;
    }
    if (response_payload) {
        char len_hex[5] = {};
        got = 0;
        while (got < 4) {
            int n = ::recv(s, len_hex + got, 4 - got, 0);
            if (n <= 0) return true;
            got += n;
        }
        unsigned plen = 0;
        if (std::sscanf(len_hex, "%04x", &plen) == 1 && plen > 0 && plen < 65536) {
            response_payload->resize(plen);
            got = 0;
            while (got < static_cast<int>(plen)) {
                int n = ::recv(s, response_payload->data() + got, static_cast<int>(plen) - got, 0);
                if (n <= 0) break;
                got += n;
            }
            response_payload->resize(static_cast<std::size_t>(got));
        }
    }
    return true;
}

} // namespace

AndroidMirrorServer::AndroidMirrorServer() = default;
AndroidMirrorServer::~AndroidMirrorServer() { stop(); }

std::string AndroidMirrorServer::last_status() const {
    std::lock_guard<std::mutex> lock(status_mtx_);
    return last_status_;
}

bool AndroidMirrorServer::start(uint16_t port) {
    if (running_.load()) return true;

    listen_sock_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ != INVALID_SOCK) {
        int on = 1;
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&on), sizeof(on));
        int v6only = 0;
        ::setsockopt(listen_sock_, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr   = in6addr_any;
        addr.sin6_port   = htons(port);
        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            ::listen(listen_sock_, 2) == 0) {
            listen_port_.store(port, std::memory_order_relaxed);
            running_.store(true);
            accept_thread_ = std::thread(&AndroidMirrorServer::accept_loop, this);
            LOG_INFO << "AndroidMirrorServer listening on TCP port " << port << " (IPv4+IPv6)";
            return true;
        }
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
    }

    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCK) return false;
    int on = 1;
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in addr4{};
    addr4.sin_family      = AF_INET;
    addr4.sin_addr.s_addr = htonl(INADDR_ANY);
    addr4.sin_port        = htons(port);
    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4)) != 0 ||
        ::listen(listen_sock_, 2) != 0) {
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }
    listen_port_.store(port, std::memory_order_relaxed);
    running_.store(true);
    accept_thread_ = std::thread(&AndroidMirrorServer::accept_loop, this);
    LOG_INFO << "AndroidMirrorServer listening on TCP port " << port << " (IPv4)";
    return true;
}

void AndroidMirrorServer::stop() {
    if (!running_.exchange(false)) return;
    disconnect_active();
    if (accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard<std::mutex> lock(worker_mtx_);
        if (adb_thread_.joinable()) adb_thread_.join();
        if (stream_thread_.joinable()) stream_thread_.join();
    }
    if (listen_sock_ != INVALID_SOCK) {
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
    }
}

void AndroidMirrorServer::disconnect_active() {
    std::lock_guard<std::mutex> lock(worker_mtx_);
    if (active_sock_ != INVALID_SOCK) {
#if defined(_WIN32)
        ::shutdown(active_sock_, SD_BOTH);
#else
        ::shutdown(active_sock_, SHUT_RDWR);
#endif
    }
}

bool AndroidMirrorServer::connect_wireless_async(const std::string& ip_or_host_port) {
    if (ip_or_host_port.empty()) return false;
    std::string trimmed = ip_or_host_port;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    if (trimmed.empty()) return false;

    std::lock_guard<std::mutex> lock(worker_mtx_);
    if (adb_thread_.joinable()) adb_thread_.join();
    adb_thread_ = std::thread(&AndroidMirrorServer::adb_connect_worker,
                              this, trimmed, "Android Wireless", false);
    return true;
}

bool AndroidMirrorServer::connect_usb_adb_async(const std::string& serial,
                                                const std::string& model_name) {
    if (serial.empty() || active_streaming_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(worker_mtx_);
    if (adb_thread_.joinable()) adb_thread_.join();
    adb_thread_ = std::thread(&AndroidMirrorServer::adb_connect_worker,
                              this, serial, model_name.empty() ? "Android USB" : model_name, true);
    return true;
}

void AndroidMirrorServer::adb_connect_worker(std::string target,
                                             std::string model_name,
                                             bool is_usb) {
    {
        std::lock_guard<std::mutex> lock(status_mtx_);
        last_status_ = is_usb ? ("Connecting Android USB (" + target + ")...")
                              : ("Connecting Android Wireless (" + target + ")...");
    }

    if (!is_usb) {
        // 1. First check if the Android device is running a direct H.264 sender on target:7001
        std::string host = target;
        uint16_t custom_port = 5555;
        const auto colon = host.rfind(':');
        if (colon != std::string::npos) {
            try {
                custom_port = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
                host = host.substr(0, colon);
            } catch (...) {}
        }

        if (custom_port == 7001) {
            socket_t direct = connect_tcp_ipv4(host, 7001, 2000);
            if (direct != INVALID_SOCK) {
                LOG_NOTICE << "AndroidMirrorServer: connected directly to Android sender at "
                           << host << ":7001";
                stream_worker(direct, host, model_name, false);
                return;
            }
        }

        // 2. Otherwise ask the local ADB server (127.0.0.1:5037) to connect to host:custom_port
        const std::string adb_target = host + ":" + std::to_string(custom_port);
        socket_t ctrl = connect_tcp_ipv4("127.0.0.1", 5037, 1500);
        if (ctrl == INVALID_SOCK) {
            std::lock_guard<std::mutex> lock(status_mtx_);
            last_status_ = "ADB service (127.0.0.1:5037) not reachable; listening on TCP :7001";
            LOG_WARN << "AndroidMirrorServer: " << last_status_;
            return;
        }
        std::string connect_reply;
        adb_command(ctrl, "host:connect:" + adb_target, &connect_reply);
        ap::net::close_socket(ctrl);
        target = adb_target;
    }

    // Open an ADB transport socket to the device and launch a low-latency raw H.264 stream
    socket_t stream_sock = connect_tcp_ipv4("127.0.0.1", 5037, 2000);
    if (stream_sock == INVALID_SOCK) return;

    const std::string transport_cmd = target.empty()
        ? "host:transport-any"
        : ("host:transport:" + target);
    if (!adb_command(stream_sock, transport_cmd, nullptr)) {
        ap::net::close_socket(stream_sock);
        std::lock_guard<std::mutex> lock(status_mtx_);
        last_status_ = "ADB device transport failed for " + target;
        return;
    }

    // Request continuous H.264 elementary stream output to stdout via ADB shell channel
    const std::string shell_cmd =
        "shell:screenrecord --bit-rate 16000000 --output-format=h264 -";
    if (!adb_command(stream_sock, shell_cmd, nullptr)) {
        ap::net::close_socket(stream_sock);
        std::lock_guard<std::mutex> lock(status_mtx_);
        last_status_ = "Failed to start Android H.264 screenrecord stream";
        return;
    }

    LOG_NOTICE << "AndroidMirrorServer: ADB H.264 stream established with " << target
               << (is_usb ? " [USB]" : " [Wi-Fi]");
    stream_worker(stream_sock, target, model_name, is_usb);
}

void AndroidMirrorServer::accept_loop() {
    ap::platform::raise_current_thread_priority();
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock_, &rfds);
        timeval tv{0, 150000};
        int r = ::select(static_cast<int>(listen_sock_) + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0 || !running_.load()) continue;

        sockaddr_storage peer{};
#if defined(_WIN32)
        int plen = sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        socket_t client = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client == INVALID_SOCK) continue;

        char ip[INET6_ADDRSTRLEN] = {};
        if (peer.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&peer);
            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                in_addr a{};
                a.s_addr = reinterpret_cast<const uint32_t*>(&in6->sin6_addr)[3];
                inet_ntop(AF_INET, &a, ip, sizeof(ip));
            } else {
                inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
            }
        } else if (peer.ss_family == AF_INET) {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&peer);
            inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
        }

        const std::string peer_ip = ip[0] ? ip : "127.0.0.1";
        const bool is_usb_loopback = (peer_ip == "127.0.0.1" || peer_ip == "::1");
        LOG_NOTICE << "AndroidMirrorServer: accepted connection from " << peer_ip;

        disconnect_active();
        {
            std::lock_guard<std::mutex> lock(worker_mtx_);
            if (stream_thread_.joinable()) stream_thread_.join();
            stream_thread_ = std::thread(&AndroidMirrorServer::stream_worker,
                                         this, client, peer_ip,
                                         is_usb_loopback ? "Android USB" : "Android Wireless",
                                         is_usb_loopback);
        }
    }
}

void AndroidMirrorServer::stream_worker(socket_t client,
                                        std::string peer_name,
                                        std::string model_name,
                                        bool is_usb) {
    ap::platform::raise_current_thread_priority();
    {
        std::lock_guard<std::mutex> lock(worker_mtx_);
        active_sock_ = client;
    }
    active_streaming_.store(true, std::memory_order_release);
    set_socket_timeout(client, kRecvTimeoutMs);
    ap::net::enable_tcp_keepalive(client, 10, 3, 6);

    const std::string session_id = "android-" + peer_name;
    if (renderer_) {
        ap::video::VideoRenderer::DeviceInfo dev;
        dev.peer_ip    = peer_name;
        dev.session_id = session_id;
        dev.kind       = is_usb ? "USB" : "Android";
        dev.name       = model_name + " (" + peer_name + ")";
        dev.model      = is_usb ? "Android USB Tunnel" : "Android Wireless";
        renderer_->set_active_device(std::move(dev));
        renderer_->begin_mirror_link();
    }

    struct StreamCleanup {
        AndroidMirrorServer* self;
        socket_t sock;
        std::string sid;
        ~StreamCleanup() {
            if (self->renderer_) {
                self->renderer_->end_mirror_link();
                self->renderer_->clear_session_if_active(sid);
            }
            {
                std::lock_guard<std::mutex> lock(self->worker_mtx_);
                if (self->active_sock_ == sock) self->active_sock_ = INVALID_SOCK;
            }
            ap::net::close_socket(sock);
            self->active_streaming_.store(false, std::memory_order_release);
        }
    } cleanup{this, client, session_id};

    const bool use_hwaccel = live_ ? live_->mirror_hwaccel.load() : true;
    ap::video::H264Decoder decoder;
    AVBufferRef* gpu_dev = (use_hwaccel && renderer_) ? renderer_->gpu_device_ref() : nullptr;
    decoder.init(use_hwaccel, gpu_dev);
    av_buffer_unref(&gpu_dev);

    ap::audio::SdlAudioOutput audio_out;
    bool audio_started = false;
    ap::audio::AacDecoder aac_decoder;
    bool aac_ready = false;

    std::vector<uint8_t> cached_sps;
    std::vector<uint8_t> cached_pps;
    int last_sps_w = 0, last_sps_h = 0;

    auto feed_annexb_nal = [&](const uint8_t* nal_with_sc, std::size_t total_len,
                               const uint8_t* raw_nal, std::size_t raw_len,
                               int64_t origin_ns) {
        if (raw_len == 0) return;
        const uint8_t nal_type = raw_nal[0] & 0x1F;
        if (nal_type == 7) { // SPS
            cached_sps.assign(raw_nal, raw_nal + raw_len);
            ap::airplay::SpsInfo info{};
            if (ap::airplay::parse_h264_sps(raw_nal, raw_len, info)) {
                if (info.width != last_sps_w || info.height != last_sps_h) {
                    LOG_NOTICE << "AndroidMirrorServer: SPS resolution/orientation -> "
                               << info.width << "x" << info.height;
                    last_sps_w = info.width;
                    last_sps_h = info.height;
                }
            }
            if (!cached_pps.empty()) {
                auto avcc = build_avcc_from_sps_pps(cached_sps, cached_pps);
                if (!avcc.empty()) {
                    decoder.set_parameter_sets_from_avcc(avcc.data(), avcc.size());
                }
            }
            return;
        }
        if (nal_type == 8) { // PPS
            cached_pps.assign(raw_nal, raw_nal + raw_len);
            if (!cached_sps.empty()) {
                auto avcc = build_avcc_from_sps_pps(cached_sps, cached_pps);
                if (!avcc.empty()) {
                    decoder.set_parameter_sets_from_avcc(avcc.data(), avcc.size());
                }
            }
            return;
        }
        if (nal_type == 5 || nal_type == 1) {
            const bool is_idr = (nal_type == 5);
            bool got_frame = false;
            int dw = 0, dh = 0;
            decoder.decode(nal_with_sc, total_len, is_idr, got_frame, dw, dh);
            if (got_frame && renderer_) {
                if (const AVFrame* af = decoder.current_avframe()) {
                    renderer_->push_avframe(af, origin_ns);
                }
            }
        }
    };

    // Peek the first 4 bytes to auto-detect WAPK framed protocol vs raw Annex-B stream
    std::vector<uint8_t> stream_buf;
    stream_buf.reserve(256 * 1024);
    std::array<uint8_t, 32768> chunk{};
    bool protocol_decided = false;
    bool is_wapk_framed   = false;

    while (running_.load()) {
        if (resync_requested_.exchange(false, std::memory_order_acq_rel)) {
            decoder.resync("Android stream manual/auto IDR resync");
        }

        int n = ::recv(client, reinterpret_cast<char*>(chunk.data()),
                       static_cast<int>(chunk.size()), 0);
        if (n == 0) {
            LOG_NOTICE << "AndroidMirrorServer: peer closed stream (" << peer_name << ")";
            break;
        }
        if (n < 0) {
            if (socket_timed_out()) continue;
            LOG_WARN << "AndroidMirrorServer: socket recv error (" << peer_name << ")";
            break;
        }

        const int64_t origin_ns = ap::video::VideoRenderer::steady_now_ns();
        if (renderer_) {
            renderer_->note_mirror_data();
            renderer_->record_payload_bytes(static_cast<std::size_t>(n));
        }

        // Convert CRLF if an older Windows ADB shell mangled binary stdout (0x0D 0x0A -> 0x0A)
        stream_buf.insert(stream_buf.end(), chunk.data(), chunk.data() + n);

        if (!protocol_decided && stream_buf.size() >= 4) {
            is_wapk_framed = (stream_buf[0] == 'W' && stream_buf[1] == 'A' &&
                              stream_buf[2] == 'P' && stream_buf[3] == 'K');
            protocol_decided = true;
            LOG_INFO << "AndroidMirrorServer: stream format = "
                     << (is_wapk_framed ? "WAPK Framed (Video+Audio+Rotation)" : "Raw Annex-B H.264");
        }
        if (!protocol_decided) continue;

        if (is_wapk_framed) {
            while (stream_buf.size() >= kWapkHeaderSize) {
                if (std::memcmp(stream_buf.data(), "WAPK", 4) != 0) {
                    LOG_WARN << "AndroidMirrorServer: WAPK frame magic lost, closing";
                    return;
                }
                const uint8_t pkt_type = stream_buf[4];
                const uint8_t flags    = stream_buf[5];
                const uint16_t sr_div  = read_u16_le(stream_buf.data() + 6);
                const uint32_t plen    = read_u32_le(stream_buf.data() + 8);
                if (plen > kMaxFrameBytes) {
                    LOG_ERROR << "AndroidMirrorServer: WAPK payload too large (" << plen << ")";
                    return;
                }
                if (stream_buf.size() < kWapkHeaderSize + plen) break;

                const uint8_t* payload = stream_buf.data() + kWapkHeaderSize;
                if (pkt_type == 1 || pkt_type == 2) { // H.264 / HEVC video
                    std::size_t sc_len = 0;
                    const auto sc_pos = find_annexb_start_code(payload, plen, 0, sc_len);
                    if (sc_pos != std::string::npos) {
                        std::size_t cur = sc_pos;
                        std::size_t cur_sc = sc_len;
                        while (cur != std::string::npos) {
                            std::size_t next_sc = 0;
                            const auto next = find_annexb_start_code(
                                payload, plen, cur + cur_sc, next_sc);
                            const std::size_t end = (next == std::string::npos) ? plen : next;
                            feed_annexb_nal(payload + cur, end - cur,
                                            payload + cur + cur_sc, end - (cur + cur_sc),
                                            origin_ns);
                            cur = next;
                            cur_sc = next_sc;
                        }
                    } else {
                        const bool is_idr = (flags & 0x01) != 0;
                        bool got_frame = false;
                        int dw = 0, dh = 0;
                        decoder.decode(payload, plen, is_idr, got_frame, dw, dh);
                        if (got_frame && renderer_) {
                            if (const AVFrame* af = decoder.current_avframe()) {
                                renderer_->push_avframe(af, origin_ns);
                            }
                        }
                    }
                } else if (pkt_type == 3 && plen >= 4) { // PCM16 stereo
                    const int sample_rate = (sr_div > 0) ? (static_cast<int>(sr_div) * 100) : 48000;
                    if (!audio_started) {
                        audio_started = audio_out.start(sample_rate, 2);
                    }
                    const auto* pcm = reinterpret_cast<const int16_t*>(payload);
                    const int sample_count = static_cast<int>(plen / sizeof(int16_t));
                    if (audio_started) audio_out.push(pcm, sample_count);
                    if (renderer_) renderer_->push_audio_pcm(pcm, sample_count, sample_rate, 2);
                } else if (pkt_type == 4 && plen > 0) { // AAC-LC audio
                    const int sample_rate = (sr_div > 0) ? (static_cast<int>(sr_div) * 100) : 48000;
                    if (!aac_ready) {
                        ap::audio::AacDecoder::Config cfg{};
                        cfg.ct = 3; // AAC-LC
                        cfg.sample_rate = sample_rate;
                        cfg.channels = 2;
                        cfg.spf = 1024;
                        aac_ready = aac_decoder.init(cfg);
                    }
                    if (!audio_started) {
                        audio_started = audio_out.start(sample_rate, 2);
                    }
                    if (aac_ready && aac_decoder.decode(payload, static_cast<int>(plen)) > 0) {
                        std::array<int16_t, 4096> pcm_buf{};
                        int pulled = 0;
                        while ((pulled = aac_decoder.pull_pcm_s16(
                                    pcm_buf.data(), static_cast<int>(pcm_buf.size()))) > 0) {
                            if (audio_started) audio_out.push(pcm_buf.data(), pulled);
                            if (renderer_) renderer_->push_audio_pcm(
                                pcm_buf.data(), pulled, sample_rate, 2);
                        }
                    }
                } else if (pkt_type == 5 && live_) { // Device orientation metadata
                    const int rot = (flags >> 1) & 0x03;
                    if (live_->video_rotation.load() != -1) {
                        live_->video_rotation.store(rot);
                    }
                }

                stream_buf.erase(stream_buf.begin(),
                                 stream_buf.begin() + kWapkHeaderSize + plen);
            }
        } else {
            // Raw Annex-B H.264 stream parser: extract complete NAL units delimited by start codes
            std::size_t sc1_len = 0;
            std::size_t first = find_annexb_start_code(
                stream_buf.data(), stream_buf.size(), 0, sc1_len);
            if (first == std::string::npos) {
                if (stream_buf.size() > 65536) {
                    stream_buf.erase(stream_buf.begin(), stream_buf.end() - 4);
                }
                continue;
            }
            if (first > 0) {
                stream_buf.erase(stream_buf.begin(),
                                 stream_buf.begin() + static_cast<std::ptrdiff_t>(first));
            }
            while (!stream_buf.empty()) {
                std::size_t cur_sc_len = 0;
                const auto cur = find_annexb_start_code(
                    stream_buf.data(), stream_buf.size(), 0, cur_sc_len);
                if (cur != 0) break;

                std::size_t next_sc_len = 0;
                const auto next = find_annexb_start_code(
                    stream_buf.data(), stream_buf.size(), cur_sc_len, next_sc_len);
                if (next == std::string::npos) break; // wait for next start code to complete NAL

                const uint8_t* nal_ptr = stream_buf.data();
                const std::size_t total_nal_len = next;
                // Convert 3-byte start code (00 00 01) to 4-byte (00 00 00 01) if needed
                if (cur_sc_len == 3) {
                    std::vector<uint8_t> aligned(total_nal_len + 1);
                    aligned[0] = 0x00;
                    std::memcpy(aligned.data() + 1, nal_ptr, total_nal_len);
                    feed_annexb_nal(aligned.data(), aligned.size(),
                                    aligned.data() + 4, aligned.size() - 4, origin_ns);
                } else {
                    feed_annexb_nal(nal_ptr, total_nal_len,
                                    nal_ptr + 4, total_nal_len - 4, origin_ns);
                }
                stream_buf.erase(stream_buf.begin(),
                                 stream_buf.begin() + static_cast<std::ptrdiff_t>(next));
            }
        }
    }
}

} // namespace ap::android
