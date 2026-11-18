// Checks how H264Decoder behaves when the stream goes bad and then recovers.
//
// A small real H.264 stream (IDR every 30 frames) is produced with libx264
// and fed through the same entry points the mirror listener uses. The test
// then injects garbage and verifies that the decoder
//   * notices the failure and resynchronises by itself,
//   * does not stay frozen: pictures come back at the next IDR,
//   * does not stay frozen when no IDR ever arrives either (time limit).
// If this FFmpeg build has no libx264 encoder the test is skipped.

#include "h264_test_stream.h"
#include "video/h264_decoder.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Run { int frames = 0; };
bool feed(ap::video::H264Decoder& d, const std::vector<uint8_t>& data, bool idr, Run& r) {
    bool got = false; int w = 0, h = 0;
    d.decode(data.data(), data.size(), idr, got, w, h);
    if (got) ++r.frames;
    return got;
}

} // namespace

int main() {
    std::vector<uint8_t> avcc;
    std::vector<h264test::Packet> stream;
    if (!h264test::make_stream(180, avcc, stream)) {
        std::printf("SKIP: this FFmpeg build has no libx264 encoder\n");
        return 0;
    }
    std::printf("stream: %zu packets, IDR at", stream.size());
    for (size_t i = 0; i < stream.size(); ++i) if (stream[i].idr) std::printf(" %zu", i);
    std::printf("\n");

    ap::video::H264Decoder dec;
    check(dec.init(false, nullptr), "decoder opens (software)");
    check(dec.set_parameter_sets_from_avcc(avcc.data(), avcc.size()), "config accepted");
    check(!dec.using_hardware(), "software mode reported");
    check(dec.fallback_to_software("test"), "fallback in software mode is a no-op");

    // 1. Healthy stream.
    Run r1;
    for (size_t i = 0; i < 60; ++i) feed(dec, stream[i].annexb, stream[i].idr, r1);
    check(r1.frames == 60, "60 healthy packets -> 60 pictures");

    // 2. Garbage in the middle of the stream.
    std::mt19937 rng(5);
    Run rg;
    for (int i = 0; i < 40; ++i) {
        std::vector<uint8_t> junk = {0, 0, 0, 1, 0x41};
        for (int k = 0; k < 200; ++k) junk.push_back(static_cast<uint8_t>(rng()));
        feed(dec, junk, false, rg);
    }
    auto st = dec.stats();
    std::printf("      after garbage: errors=%llu resyncs=%llu skipped=%llu\n",
                (unsigned long long)st.errors, (unsigned long long)st.resyncs,
                (unsigned long long)st.skipped);
    check(st.resyncs >= 1, "decoder resynchronised after repeated failures");

    // 3. Stream continues: inter frames are skipped until the next IDR, then
    //    pictures come back and stay.
    Run rr;
    size_t first_pic = 0;
    for (size_t i = 61; i < stream.size(); ++i) {
        if (feed(dec, stream[i].annexb, stream[i].idr, rr) && !first_pic) first_pic = i;
    }
    std::printf("      first picture after recovery at packet %zu, total %d\n", first_pic, rr.frames);
    check(first_pic != 0 && stream[first_pic].idr, "picture returns at the next IDR");
    check(rr.frames >= static_cast<int>(stream.size() - first_pic) - 1, "no pictures lost after recovery");

    // 4. No IDR ever arrives after a resync: must not stay frozen forever.
    ap::video::H264Decoder d2;
    d2.init(false, nullptr);
    d2.set_parameter_sets_from_avcc(avcc.data(), avcc.size());
    Run r2;
    for (size_t i = 0; i < 10; ++i) feed(d2, stream[i].annexb, stream[i].idr, r2);
    d2.resync("test");
    const auto before = d2.stats();
    for (size_t i = 11; i < 20; ++i) feed(d2, stream[i].annexb, stream[i].idr, r2);   // skipped
    check(d2.stats().skipped > before.skipped, "inter frames skipped while waiting for IDR");
    std::this_thread::sleep_for(std::chrono::milliseconds(1700));
    const auto mid = d2.stats();
    for (size_t i = 20; i < 29; ++i) feed(d2, stream[i].annexb, stream[i].idr, r2);
    const auto after = d2.stats();
    check(after.skipped == mid.skipped, "stops skipping after the wait limit");
    check(after.packets > mid.packets, "inter frames are fed to the decoder again");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
