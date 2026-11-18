#pragma once
// Builds a tiny real H.264 stream with libx264 for the decoder/mirror tests.
// Packets carry only slice NAL units (Annex-B), like the AirPlay mirror
// payload after AVCC->Annex-B conversion; SPS/PPS come back as an avcC blob.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstdint>
#include <utility>
#include <vector>

namespace h264test {

struct Packet { std::vector<uint8_t> annexb; bool idr = false; };

// Split an Annex-B buffer into NAL units (without start codes).
inline std::vector<std::pair<const uint8_t*, size_t>> split_nals(const uint8_t* d, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    size_t i = 0, start = 0;
    bool have = false;
    while (i + 3 <= n) {
        const bool sc3 = d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
        const bool sc4 = i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1;
        if (sc3 || sc4) {
            if (have) out.emplace_back(d + start, i - start);
            i += sc4 ? 4 : 3;
            start = i;
            have = true;
        } else {
            ++i;
        }
    }
    if (have && start < n) out.emplace_back(d + start, n - start);
    return out;
}

// Encode `count` frames. Fills the avcC config blob and one packet per frame
// that carries only the slice NAL units, as the AirPlay mirror stream does.
inline bool make_stream(int count, std::vector<uint8_t>& avcc, std::vector<Packet>& packets) {
    const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
    if (!enc) return false;
    AVCodecContext* c = avcodec_alloc_context3(enc);
    c->width = 320; c->height = 240;
    c->time_base = {1, 30}; c->framerate = {30, 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->gop_size = 30; c->max_b_frames = 0;
    av_opt_set(c->priv_data, "preset", "ultrafast", 0);
    av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    av_opt_set(c->priv_data, "x264-params", "keyint=30:min-keyint=30:scenecut=0", 0);
    if (avcodec_open2(c, enc, nullptr) < 0) { avcodec_free_context(&c); return false; }

    AVFrame* f = av_frame_alloc();
    f->format = c->pix_fmt; f->width = c->width; f->height = c->height;
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();

    std::vector<uint8_t> sps, pps;
    auto handle = [&](AVPacket* p) {
        Packet out;
        out.idr = (p->flags & AV_PKT_FLAG_KEY) != 0;
        for (auto [nal, size] : split_nals(p->data, p->size)) {
            const int type = nal[0] & 0x1f;
            if (type == 7) sps.assign(nal, nal + size);
            else if (type == 8) pps.assign(nal, nal + size);
            else if (type == 1 || type == 5) {
                static const uint8_t sc[4] = {0, 0, 0, 1};
                out.annexb.insert(out.annexb.end(), sc, sc + 4);
                out.annexb.insert(out.annexb.end(), nal, nal + size);
            }
        }
        if (!out.annexb.empty()) packets.push_back(std::move(out));
    };
    for (int i = 0; i < count; ++i) {
        av_frame_make_writable(f);
        for (int y = 0; y < c->height; ++y)
            for (int x = 0; x < c->width; ++x)
                f->data[0][y * f->linesize[0] + x] = static_cast<uint8_t>(x + y + i * 3);
        for (int y = 0; y < c->height / 2; ++y)
            for (int x = 0; x < c->width / 2; ++x) {
                f->data[1][y * f->linesize[1] + x] = static_cast<uint8_t>(128 + i);
                f->data[2][y * f->linesize[2] + x] = static_cast<uint8_t>(128 - i);
            }
        f->pts = i;
        if (avcodec_send_frame(c, f) < 0) break;
        while (avcodec_receive_packet(c, pkt) == 0) { handle(pkt); av_packet_unref(pkt); }
    }
    avcodec_send_frame(c, nullptr);
    while (avcodec_receive_packet(c, pkt) == 0) { handle(pkt); av_packet_unref(pkt); }
    av_packet_free(&pkt); av_frame_free(&f); avcodec_free_context(&c);
    if (sps.size() < 4 || pps.empty()) return false;

    // avcC: version, profile, compat, level, 0xFF, 0xE1, sps, 1 pps.
    avcc = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1,
            static_cast<uint8_t>(sps.size() >> 8), static_cast<uint8_t>(sps.size())};
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);
    avcc.push_back(static_cast<uint8_t>(pps.size() >> 8));
    avcc.push_back(static_cast<uint8_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return true;
}


} // namespace h264test
