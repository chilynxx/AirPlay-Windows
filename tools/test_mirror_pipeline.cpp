// End-to-end test of the mirror receive path (TCP -> frame parser -> AES-CTR
// -> NAL conversion -> H.264 decode) using a real, synthetic H.264 stream.
//
// Scenarios:
//   1. a healthy stream decodes completely;
//   2. a pause of a second or so in the middle of a frame (a Wi-Fi hiccup)
//      does NOT end the session and nothing is lost;
//   3. garbage frames make the decoder resynchronise by itself and the
//      picture returns at the next IDR;
//   4. a completely silent sender is detected and the stream is closed
//      instead of leaving the last picture frozen forever;
//   5. the listener keeps accepting new connections afterwards.
// Skipped when this FFmpeg build has no libx264 encoder.

#define SDL_MAIN_HANDLED   // keep our own main() when SDL.h is pulled in on Windows
#include "airplay/mirror_listener.h"
#include "crypto/mirror_decrypt.h"
#include "h264_test_stream.h"
#include "log.h"
#include "net/socket.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// The listener only touches the renderer through these three calls, and this
// test attaches no renderer, so empty stand-ins keep the test free of SDL/GPU.
namespace ap::video {
AVBufferRef* VideoRenderer::gpu_device_ref() { return nullptr; }
void VideoRenderer::record_payload_bytes(std::size_t) {}
void VideoRenderer::push_avframe(const AVFrame*, int64_t) {}
} // namespace ap::video

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

socket_t connect_local(uint16_t port) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (fd == INVALID_SOCK ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
        return INVALID_SOCK;
    return fd;
}

constexpr int kHeader = 128;

std::vector<uint8_t> frame_header(uint32_t payload, uint8_t type_hi, uint8_t type_lo) {
    std::vector<uint8_t> h(kHeader, 0);
    h[0] = payload & 0xff; h[1] = (payload >> 8) & 0xff;
    h[2] = (payload >> 16) & 0xff; h[3] = (payload >> 24) & 0xff;
    h[4] = type_hi; h[5] = type_lo;
    return h;
}

// Annex-B (start-code) NAL list -> AVCC (4-byte big-endian lengths).
std::vector<uint8_t> to_avcc(const std::vector<uint8_t>& annexb) {
    std::vector<uint8_t> out;
    for (auto [nal, size] : h264test::split_nals(annexb.data(), annexb.size())) {
        out.push_back(static_cast<uint8_t>(size >> 24)); out.push_back(static_cast<uint8_t>(size >> 16));
        out.push_back(static_cast<uint8_t>(size >> 8));  out.push_back(static_cast<uint8_t>(size));
        out.insert(out.end(), nal, nal + size);
    }
    return out;
}

struct Sender {
    socket_t fd = INVALID_SOCK;
    ap::crypto::MirrorDecrypt cipher;   // CTR: "decrypt" on plaintext == encrypt

    bool raw(const std::vector<uint8_t>& d) {
        return ap::net::send_all(fd, d.data(), static_cast<int>(d.size())) > 0;
    }
    bool sps_pps(const std::vector<uint8_t>& avcc) {
        auto h = frame_header(static_cast<uint32_t>(avcc.size()), 0x01, 0x00);
        h.insert(h.end(), avcc.begin(), avcc.end());
        return raw(h);
    }
    bool heartbeat() { return raw(frame_header(0, 0x05, 0x00)); }   // stream report
    bool beat() { return raw(frame_header(0, 0x02, 0x00)); }        // heartbeat

    std::vector<uint8_t> video_bytes(std::vector<uint8_t> avcc_payload, bool idr) {
        cipher.decrypt(avcc_payload.data(), static_cast<int>(avcc_payload.size()));
        auto h = frame_header(static_cast<uint32_t>(avcc_payload.size()), 0x00, idr ? 0x10 : 0x00);
        h.insert(h.end(), avcc_payload.begin(), avcc_payload.end());
        return h;
    }
    bool video(const h264test::Packet& p) { return raw(video_bytes(to_avcc(p.annexb), p.idr)); }
};

} // namespace

int main() {
    ap::net::global_init();
    ap::set_log_enabled(false);
    const auto log_path = std::filesystem::temp_directory_path() / "airplay_mirror_test.log";
    std::filesystem::remove(log_path);
    ap::set_log_file(log_path.string());

    std::vector<uint8_t> avcc;
    std::vector<h264test::Packet> stream;
    if (!h264test::make_stream(180, avcc, stream)) {
        std::printf("SKIP: this FFmpeg build has no libx264 encoder\n");
        return 0;
    }

    const std::vector<unsigned char> key(16, 0x42);
    const uint64_t conn_id = 123456789ULL;

    ap::airplay::MirrorListener listener;
    listener.set_idle_limit(1500ms);
    listener.set_health_period(300ms);
    listener.set_stall_warn(700ms);
    check(listener.enable_decrypt(key, conn_id, false), "decrypt + decoder initialised");
    uint16_t port = 0;
    check(listener.start(port) && port != 0, "listener started");

    Sender tx;
    check(tx.cipher.init(key, conn_id), "test sender cipher initialised");
    tx.fd = connect_local(port);
    check(tx.fd != INVALID_SOCK, "sender connected");

    // 1 + 2. Healthy stream, with a mid-frame pause that must be tolerated.
    check(tx.sps_pps(avcc), "SPS/PPS sent");
    for (size_t i = 0; i < 40; ++i) tx.video(stream[i]);
    {
        auto bytes = tx.video_bytes(to_avcc(stream[40].annexb), stream[40].idr);
        const size_t half = kHeader + (bytes.size() - kHeader) / 2;
        tx.raw({bytes.begin(), bytes.begin() + half});
        std::this_thread::sleep_for(800ms);          // less than the idle limit
        tx.raw({bytes.begin() + half, bytes.end()});
    }
    for (size_t i = 41; i < 90; ++i) { tx.video(stream[i]); std::this_thread::sleep_for(10ms); }
    check(wait_until([&] { return listener.decoded_frames() >= 90; }, 5s),
          "all 90 frames decoded, including across the 0.8 s pause");

    // 3. Garbage that still frames correctly: the decoder must resync itself.
    std::mt19937 rng(11);
    for (int i = 0; i < 20; ++i) {
        std::vector<uint8_t> avcc_junk;
        const uint32_t n = 100;
        avcc_junk = {0, 0, 0, static_cast<uint8_t>(n), 0x41};
        for (uint32_t k = 1; k < n; ++k) avcc_junk.push_back(static_cast<uint8_t>(rng()));
        tx.raw(tx.video_bytes(avcc_junk, false));
    }
    std::this_thread::sleep_for(200ms);
    const uint64_t before = listener.decoded_frames();
    for (size_t i = 90; i < 180; ++i) tx.video(stream[i]);        // IDR at 90
    check(wait_until([&] { return listener.decoded_frames() >= before + 88; }, 5s),
          "picture recovered after garbage (IDR resynchronisation)");
    std::printf("      decoded after recovery: %llu\n",
                static_cast<unsigned long long>(listener.decoded_frames() - before));

    // 3b. Connected and alive, but no video for a while (static screen or a sender
    //     whose video stalled): the log must say so, and say when pictures return.
    for (int i = 0; i < 8; ++i) { tx.beat(); std::this_thread::sleep_for(200ms); }
    const uint64_t pics_before_resume = listener.decoded_frames();
    for (size_t i = 0; i < 6; ++i) tx.video(stream[i]);              // IDR + 5 inter frames
    check(wait_until([&] { return listener.decoded_frames() >= pics_before_resume + 6; }, 5s),
          "pictures resume after a stretch with only heartbeats");
    std::this_thread::sleep_for(300ms);

    // 4. The sender goes completely silent: the stream must be closed.
    tx.heartbeat();
    char c;
#if defined(_WIN32)
    DWORD tmo = 4000;
#else
    timeval tmo{4, 0};
#endif
    setsockopt(tx.fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    const auto t0 = std::chrono::steady_clock::now();
    const int r = ::recv(tx.fd, &c, 1, 0);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("      silent sender detected after %lld ms\n", static_cast<long long>(waited));
    check(r == 0 && waited >= 1000 && waited < 4000,
          "silent sender: connection closed after the idle limit");
    ap::net::close_socket(tx.fd);

    // 5. New connections are still accepted.
    socket_t again = connect_local(port);
    check(again != INVALID_SOCK, "a new connection is accepted afterwards");
    if (again != INVALID_SOCK) ap::net::close_socket(again);

    listener.stop();

    std::ifstream in(log_path);
    std::stringstream ss; ss << in.rdbuf();
    const std::string log = ss.str();
    check(log.find("no data from the sender") != std::string::npos,
          "idle disconnect is written to the log file");
    check(log.find("decoder: resync") != std::string::npos,
          "decoder resync is written to the log file");
    check(log.find("health: mirror rx") != std::string::npos,
          "health line is written while frames are flowing");
    check(log.find("sender sent video=") != std::string::npos,
          "health line says what kinds of frames the sender sent");
    check(log.find("no new picture for") != std::string::npos,
          "a stretch without pictures is reported in the log");
    check(log.find("pictures resumed after") != std::string::npos,
          "the return of pictures is reported in the log");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
