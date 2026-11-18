#include "video/h264_decoder.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

constexpr uint8_t kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

// ---- recovery policy ---------------------------------------------------------
// After this many failed calls in a row, flush the decoder and wait for a key
// frame instead of feeding it more data it cannot use.
constexpr int kErrorsBeforeResync = 6;
// Packets fed without a single picture coming out: the decoder is stuck.
constexpr int kPacketsWithoutFrame = 120;
// While waiting for an IDR, inter frames are skipped for at most this long.
constexpr auto kMaxIdrWait = std::chrono::milliseconds(1500);
// Three resyncs within a minute: the decoder itself is broken, escalate.
constexpr auto kEscalateWindow = std::chrono::seconds(60);
constexpr std::size_t kEscalateCount = 3;
// GPU decode slower than this for this many frames in a row: use the CPU.
constexpr double kSlowGpuDecodeMs = 60.0;
constexpr int    kSlowGpuStreak   = 30;
// A GPU that failed twice recently is not trusted again for a while.
constexpr int    kDistrustFailures = 2;
constexpr auto   kDistrustDuration = std::chrono::minutes(30);

std::atomic<int>     g_hw_failures{0};
std::atomic<int64_t> g_last_hw_failure_ns{0};

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void append_annexb_nal(std::vector<uint8_t>& dst,
                       const uint8_t* nal, std::size_t size) {
    dst.insert(dst.end(), kStartCode, kStartCode + 4);
    dst.insert(dst.end(), nal, nal + size);
}

} // namespace

struct H264Decoder::Impl {
    const AVCodec*       codec       = nullptr;
    AVCodecContext*      ctx         = nullptr;
    AVPacket*            pkt         = nullptr;
    AVFrame*             frame       = nullptr;
    AVFrame*             tmp_frame   = nullptr;   // receive target, see drain()
    SwsContext*          sws         = nullptr;

    // Annex-B bytes (with leading 00 00 00 01 start code) cached from
    // the last config blob so every IDR can be prepended with its
    // parameter sets. HEVC has three (VPS+SPS+PPS), H.264 has two.
    std::vector<uint8_t> vps_annexb;   // HEVC only; empty for H.264
    std::vector<uint8_t> sps_annexb;
    std::vector<uint8_t> pps_annexb;

    AVCodecID            codec_id    = AV_CODEC_ID_H264;

    int                  last_w      = 0;
    int                  last_h      = 0;
    std::vector<uint8_t> last_rgb;       // WxHx3 filled on every successful decode
    bool                 last_is_yuv420 = false;
    uint64_t             frames_out  = 0;

    // D3D11VA outputs resident GPU textures; presentation never downloads them.
    enum class HwBackend { None, D3D11VA };

    // Per-decode scratch buffer for the Annex-B packet that gets
    // fed to libavcodec. Reused across frames; clear() preserves
    // capacity, so after the first IDR (which determines the peak
    // size) we never reallocate the heap again. Removes the per-
    // frame buf.reserve+insert+insert+insert allocation jitter.
    std::vector<uint8_t> packet_scratch;

    bool                 hwaccel_requested = false;
    AVBufferRef*         hw_device_d3d11   = nullptr;   // lazy
    AVPixelFormat        hw_pix_fmt        = AV_PIX_FMT_NONE;
    bool                 frame_is_hw       = false;
    HwBackend            backend           = HwBackend::None;

    // ---- health / recovery state -------------------------------------------
    uint64_t packets_in = 0, errors = 0, skipped = 0, resyncs = 0;
    int      consecutive_errors   = 0;
    int      packets_without_frame = 0;
    int      slow_streak          = 0;
    double   last_ms = 0, avg_ms = 0, max_ms = 0;
    bool     wait_idr     = true;   // nothing useful can be decoded before an IDR
    bool     wait_started = false;
    std::chrono::steady_clock::time_point wait_since{};
    std::deque<std::chrono::steady_clock::time_point> recent_resyncs;
    uint64_t error_logs = 0;

    void begin_wait_for_idr() {
        wait_idr = true;
        wait_started = false;
        consecutive_errors = 0;
        packets_without_frame = 0;
        slow_streak = 0;
    }

    // Pull every picture libavcodec has ready and keep the newest one in
    // `frame`. Returns true when at least one picture was produced; `err`
    // receives the first hard error (0 when none).
    bool drain(int& err) {
        bool any = false;
        err = 0;
        for (;;) {
            const int r = avcodec_receive_frame(ctx, tmp_frame);
            if (r == 0) {
                av_frame_unref(frame);
                av_frame_move_ref(frame, tmp_frame);
                ++frames_out;
                any = true;
                continue;
            }
            if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) err = r;
            break;
        }
        return any;
    }

    ~Impl() {
        if (sws)              sws_freeContext(sws);
        if (tmp_frame)        av_frame_free(&tmp_frame);
        if (frame)            av_frame_free(&frame);
        if (pkt)              av_packet_free(&pkt);
        if (ctx)              avcodec_free_context(&ctx);
        if (hw_device_d3d11)  av_buffer_unref(&hw_device_d3d11);
    }

    // get_format callback — libavcodec asks us which pixel format to
    // output, after parsing the SPS. We pick AV_PIX_FMT_D3D11 when
    // the encoder + system support it, otherwise let libavcodec
    // pick its software default (NV12 / YUV420P).
    static AVPixelFormat get_hw_format_cb(AVCodecContext* c,
                                          const AVPixelFormat* pix_fmts) {
        auto* self = static_cast<Impl*>(c->opaque);
        if (self) {
            for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                if (*p == self->hw_pix_fmt) return *p;
            }
        }
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return AV_PIX_FMT_NONE;
    }

    // Open (or re-open after a codec switch) libavcodec for `id`.
    bool open(AVCodecID id) {
        const AVCodec* c   = nullptr;
        HwBackend      bk  = HwBackend::None;
        AVBufferRef*   dev = nullptr;

        if (hwaccel_requested && hw_device_d3d11) {
            c = avcodec_find_decoder(id);
            bk = HwBackend::D3D11VA;
            dev = hw_device_d3d11;
        }
        if (!c) c = avcodec_find_decoder(id);
        if (!c) {
            LOG_ERROR << "decoder: codec " << avcodec_get_name(id)
                      << " not available in libavcodec";
            return false;
        }

        if (ctx) avcodec_free_context(&ctx);
        codec    = c;
        codec_id = id;
        ctx      = avcodec_alloc_context3(c);
        if (!ctx) return false;

        // Use the render pipeline's D3D11 decode device.
        if (bk != HwBackend::None && dev) {
            ctx->hw_device_ctx = av_buffer_ref(dev);
            hw_pix_fmt      = AV_PIX_FMT_D3D11;
            ctx->get_format = &Impl::get_hw_format_cb;
            ctx->opaque     = this;
        }
        bool hw_enabled = (bk != HwBackend::None);
        backend = bk;

        // Mirror is a real-time interactive stream — every ms of
        // decoder buffering is felt as input lag (camera-app demo:
        // pan the phone, see the mirror trail behind).
        // AV_CODEC_FLAG_LOW_DELAY tells libavcodec to skip the
        // display-order reorder buffer (HEVC's "has_b_frames"
        // lookahead in particular) and emit each frame the moment
        // it's decoded.
        //
        // We previously also set AV_CODEC_FLAG2_FAST for an extra
        // few ms, but it allowed libavcodec to skip the in-stream
        // SPS re-evaluation that mid-session resolution changes
        // depend on (iPhone rotated portrait↔landscape: new SPS
        // with new dims, but the decoder kept its old internal
        // state and the renderer never saw the orientation flip).
        // Dropped — the LOW_DELAY win was the bigger one anyway.
        ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;

        // Mirror packets do not carry synthesized presentation timestamps.
        ctx->pkt_timebase = AVRational{ 1, AV_TIME_BASE };

        // Default libavcodec threading (FF_THREAD_FRAME) decodes
        // multiple frames in parallel — great throughput, but it
        // requires keeping N-1 in-flight frames in a queue, which
        // shows up as input lag. FF_THREAD_SLICE parallelises
        // within a single frame instead, so the decode pipeline
        // never holds more than one frame at a time. Software
        // mode gains the most; D3D11VA does its own GPU threading
        // and is roughly indifferent.
        ctx->thread_type  = FF_THREAD_SLICE;
        ctx->thread_count = 0;   // 0 = auto (one per CPU core)

        // Capacity for decoder output, the latest-wins render slot and
        // the bounded recording worker. Pool capacity does not queue frames
        // for presentation; insufficient capacity can stall the decoder.
        ctx->extra_hw_frames = 8;

        if (avcodec_open2(ctx, c, nullptr) != 0) {
            LOG_ERROR << "decoder: avcodec_open2 failed for "
                      << avcodec_get_name(id) << " ("
                      << (c->name ? c->name : "?") << ')';
            // If we tried HW and it failed at open time, fall back
            // to software so the user still sees something.
            if (hw_enabled) {
                LOG_WARN << "decoder: HW open failed — retrying software";
                avcodec_free_context(&ctx);
                c = avcodec_find_decoder(id);
                if (!c) return false;
                ctx = avcodec_alloc_context3(c);
                if (!ctx) return false;
                ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                ctx->thread_type  = FF_THREAD_SLICE;
                ctx->thread_count = 0;
                if (avcodec_open2(ctx, c, nullptr) != 0) return false;
                hw_enabled = false;
                backend    = HwBackend::None;
                hw_pix_fmt = AV_PIX_FMT_NONE;
            } else {
                return false;
            }
        }
        const char* tag =
            (backend == HwBackend::D3D11VA) ? "[D3D11VA]"     :
                                              "[software]";
        const char* tt =
            (ctx->active_thread_type == FF_THREAD_SLICE) ? "slice" :
            (ctx->active_thread_type == FF_THREAD_FRAME) ? "frame" :
            (ctx->active_thread_type ==
                (FF_THREAD_SLICE | FF_THREAD_FRAME))     ? "slice+frame" :
                                                            "none";
        LOG_NOTICE << "decoder ready: " << avcodec_get_name(id)
                 << " via " << (c->name ? c->name : "?")
                 << " (libavcodec " << LIBAVCODEC_IDENT << ") "
                 << tag << " [LOW_DELAY]"
                 << " threads=" << ctx->thread_count
                 << '/' << tt;
        return true;
    }
};

H264Decoder::H264Decoder()  : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::init(bool hwaccel, AVBufferRef* render_device) {
    impl_->pkt = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->tmp_frame = av_frame_alloc();
    if (!impl_->pkt || !impl_->frame || !impl_->tmp_frame) return false;
    if (hwaccel && hardware_distrusted()) {
        LOG_WARN << "decoder: GPU decoding failed recently in this process — "
                    "starting in software mode";
        hwaccel = false;
    }
    // Direct rendering requires the GPU pipeline's D3D11 decode device.
    impl_->hwaccel_requested = hwaccel && render_device;
    if (impl_->hwaccel_requested)
        impl_->hw_device_d3d11 = av_buffer_ref(render_device);
    return impl_->open(AV_CODEC_ID_H264);
}

namespace {

// Parse avcC (ISO 14496-15 §5.2.4.1.1) and populate sps/pps Annex-B.
// Returns true only when the blob is fully consumed — partial parses
// get rejected so we can fall through to the hvcC attempt.
bool parse_avcc(const uint8_t* blob, std::size_t size,
                std::vector<uint8_t>& sps_out,
                std::vector<uint8_t>& pps_out) {
    if (!blob || size < 7 || blob[0] != 0x01) return false;
    const uint8_t num_sps = blob[5] & 0x1f;
    if (num_sps < 1) return false;

    std::size_t p = 6;
    sps_out.clear();
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (p + 2 > size) return false;
        const uint16_t len =
            (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(sps_out, blob + p, len);
        p += len;
    }
    if (p >= size) return false;
    const uint8_t num_pps = blob[p++];
    pps_out.clear();
    for (uint8_t i = 0; i < num_pps; ++i) {
        if (p + 2 > size) return false;
        const uint16_t len =
            (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(pps_out, blob + p, len);
        p += len;
    }
    // iOS avcC has no trailing bytes; a malformed blob with extras
    // looks superficially like hvcC and should fall through.
    return !sps_out.empty() && !pps_out.empty();
}

// Parse hvcC (ISO 14496-15 §8.3.3.1.2): header is 22 bytes, then
// numOfArrays (u8), each array is
//   u8  array_completeness(1) | reserved(1) | NAL_unit_type(6)
//   u16 numNalus
//   for each nalu: u16 length, byte[length] data
// HEVC NAL types: VPS=32, SPS=33, PPS=34.
bool parse_hvcc(const uint8_t* blob, std::size_t size,
                std::vector<uint8_t>& vps_out,
                std::vector<uint8_t>& sps_out,
                std::vector<uint8_t>& pps_out) {
    if (!blob || size < 23 || blob[0] != 0x01) return false;
    std::size_t p = 22;
    const uint8_t num_arrays = blob[p++];
    vps_out.clear(); sps_out.clear(); pps_out.clear();
    for (uint8_t i = 0; i < num_arrays; ++i) {
        if (p + 3 > size) return false;
        const uint8_t  nal_type = blob[p] & 0x3f;
        const uint16_t num_nalus =
            (static_cast<uint16_t>(blob[p + 1]) << 8) | blob[p + 2];
        p += 3;
        for (uint16_t j = 0; j < num_nalus; ++j) {
            if (p + 2 > size) return false;
            const uint16_t len =
                (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
            p += 2;
            if (p + len > size) return false;
            if (j == 0) {
                if      (nal_type == 32) append_annexb_nal(vps_out, blob + p, len);
                else if (nal_type == 33) append_annexb_nal(sps_out, blob + p, len);
                else if (nal_type == 34) append_annexb_nal(pps_out, blob + p, len);
            }
            p += len;
        }
    }
    return !vps_out.empty() && !sps_out.empty() && !pps_out.empty();
}

// Some iOS mirror flows (notably HEVC) deliver the config wrapped
// inside an MP4 VisualSampleEntry: the payload begins with
// [size:u32 BE][fourcc "avc1"|"hvc1"|"hev1"|"avc3"][...VisualSampleEntry
// fields...][child box "avcC"|"hvcC"|...]. Find the inner config
// box by scanning for its fourcc and return a pointer to the box
// content (after the 8-byte size+type header). Returns false if no
// known config child box is present.
bool find_inner_config_box(const uint8_t* blob, std::size_t size,
                           const uint8_t*& inner_blob,
                           std::size_t&    inner_size) {
    static const char* const kFourccs[] = {"avcC", "hvcC"};
    for (const char* fourcc : kFourccs) {
        // Need at least 4 bytes of size field before the fourcc.
        for (std::size_t i = 4; i + 4 <= size; ++i) {
            if (blob[i + 0] == static_cast<uint8_t>(fourcc[0]) &&
                blob[i + 1] == static_cast<uint8_t>(fourcc[1]) &&
                blob[i + 2] == static_cast<uint8_t>(fourcc[2]) &&
                blob[i + 3] == static_cast<uint8_t>(fourcc[3])) {
                const uint8_t* sz = blob + i - 4;
                const std::size_t box_size =
                    (static_cast<std::size_t>(sz[0]) << 24) |
                    (static_cast<std::size_t>(sz[1]) << 16) |
                    (static_cast<std::size_t>(sz[2]) << 8)  |
                     static_cast<std::size_t>(sz[3]);
                if (box_size >= 9 && (i - 4) + box_size <= size) {
                    inner_blob = blob + i + 4;            // after fourcc
                    inner_size = box_size - 8;            // minus header
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

// Accept either an H.264 avcC or an HEVC hvcC blob, reinit the
// decoder on codec change. Historical name kept for callers.
bool H264Decoder::set_parameter_sets_from_avcc(const uint8_t* blob,
                                               std::size_t size) {
    if (!blob || size < 7) {
        LOG_WARN << "decoder: config blob missing or too short ("
                 << size << "B)";
        return false;
    }

    // Most iOS H.264 mirror flows hand us the raw avcC blob whose
    // configurationVersion byte is 0x01. Newer HEVC mirror flows
    // wrap the config in an MP4 SampleEntry — first byte is 0x00
    // (the high byte of the box size), and the inner avcC / hvcC
    // box sits a few dozen bytes in. Detect the wrapped case and
    // unwrap once before trying the parsers.
    const uint8_t* cfg = blob;
    std::size_t    csz = size;
    if (cfg[0] != 0x01) {
        const uint8_t* inner = nullptr;
        std::size_t    isize = 0;
        if (find_inner_config_box(blob, size, inner, isize)) {
            LOG_INFO << "decoder: unwrapped MP4 SampleEntry, inner "
                     << "config box " << isize << "B";
            cfg = inner;
            csz = isize;
        } else {
            LOG_WARN << "decoder: config blob has no avcC/hvcC inner "
                        "box and no leading 0x01 (" << size << "B)";
            return false;
        }
    }
    if (cfg[0] != 0x01) {
        LOG_WARN << "decoder: inner config wrong version (0x"
                 << std::hex << static_cast<int>(cfg[0]) << ')';
        return false;
    }

    // D3D11VA and software handle in-stream SPS changes internally.
    auto reopen_for_sps_change = [&](AVCodecID id, const std::vector<uint8_t>&) {
        return impl_->codec_id == id || impl_->open(id);
    };
    // Try H.264 avcC first (common path).
    std::vector<uint8_t> new_sps, new_pps;
    if (parse_avcc(cfg, csz, new_sps, new_pps)) {
        if (!reopen_for_sps_change(AV_CODEC_ID_H264, new_sps)) {
            return false;
        }
        impl_->vps_annexb.clear();
        impl_->sps_annexb = std::move(new_sps);
        impl_->pps_annexb = std::move(new_pps);
        LOG_INFO << "decoder: H.264 SPS="
                 << (impl_->sps_annexb.size() - 4) << "B PPS="
                 << (impl_->pps_annexb.size() - 4) << "B cached";
        return true;
    }

    // Fall through to HEVC hvcC.
    std::vector<uint8_t> new_vps;
    new_sps.clear(); new_pps.clear();
    if (parse_hvcc(cfg, csz, new_vps, new_sps, new_pps)) {
        if (!reopen_for_sps_change(AV_CODEC_ID_HEVC, new_sps)) {
            return false;
        }
        impl_->vps_annexb = std::move(new_vps);
        impl_->sps_annexb = std::move(new_sps);
        impl_->pps_annexb = std::move(new_pps);
        LOG_INFO << "decoder: HEVC VPS="
                 << (impl_->vps_annexb.size() - 4) << "B SPS="
                 << (impl_->sps_annexb.size() - 4) << "B PPS="
                 << (impl_->pps_annexb.size() - 4) << "B cached";
        return true;
    }

    LOG_WARN << "decoder: config blob matched neither avcC nor hvcC ("
             << size << "B)";
    return false;
}

bool H264Decoder::decode(const uint8_t* nal_data, std::size_t nal_size,
                         bool is_idr,
                         bool& got_frame, int& width, int& height) {
    got_frame = false;
    width = height = 0;
    auto& im = *impl_;
    if (!im.ctx) return false;
    if (!nal_data || nal_size == 0) return false;

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    // At the start and after a resync, inter frames cannot be decoded. Skip
    // them, but only for a short time: a sender that rarely sends IDRs must
    // not leave the picture frozen.
    if (im.wait_idr && !is_idr) {
        if (!im.wait_started) { im.wait_started = true; im.wait_since = t0; }
        if (t0 - im.wait_since < kMaxIdrWait) { ++im.skipped; return true; }
        LOG_WARN << "decoder: no IDR within " << kMaxIdrWait.count()
                 << " ms — decoding inter frames anyway";
        im.wait_idr = false;
    }
    if (is_idr) { im.wait_idr = false; im.wait_started = false; }

    // Build the Annex-B packet in the per-decoder scratch buffer.
    // clear() preserves capacity — after the first IDR (which sets
    // the peak size) the vector never reallocates again, so each
    // subsequent frame is heap-jitter-free.
    auto& buf = im.packet_scratch;
    buf.clear();
    const std::size_t header_bytes = is_idr
        ? (im.vps_annexb.size() + im.sps_annexb.size() + im.pps_annexb.size())
        : 0;
    if (buf.capacity() < nal_size + header_bytes) {
        buf.reserve(nal_size + header_bytes);
    }
    if (is_idr) {
        if (!im.vps_annexb.empty())
            buf.insert(buf.end(), im.vps_annexb.begin(), im.vps_annexb.end());
        buf.insert(buf.end(), im.sps_annexb.begin(), im.sps_annexb.end());
        buf.insert(buf.end(), im.pps_annexb.begin(), im.pps_annexb.end());
    }
    buf.insert(buf.end(), nal_data, nal_data + nal_size);

    av_packet_unref(im.pkt);
    im.pkt->data = buf.data();
    im.pkt->size = static_cast<int>(buf.size());
    ++im.packets_in;
    ++im.packets_without_frame;

    int  hard_error = 0;
    bool got = false;
    int  ret = avcodec_send_packet(im.ctx, im.pkt);
    if (ret == AVERROR(EAGAIN)) {
        // The decoder wants its output taken before it accepts more input.
        int err = 0;
        got = im.drain(err);
        if (err) hard_error = err;
        ret = avcodec_send_packet(im.ctx, im.pkt);
    }
    if (ret < 0) hard_error = ret;
    {
        int err = 0;
        if (im.drain(err)) got = true;
        if (err && !hard_error) hard_error = err;
    }

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    im.last_ms = ms;
    im.avg_ms  = im.avg_ms <= 0 ? ms : im.avg_ms * 0.9 + ms * 0.1;
    if (ms > im.max_ms) im.max_ms = ms;

    if (got) {
        const AVFrame* f = im.frame;
        width   = f->width;
        height  = f->height;
        im.last_w = width;
        im.last_h = height;
        // Hardware output carries a frame-pool reference; software output does not.
        im.frame_is_hw = (f->hw_frames_ctx != nullptr);
        // GPU surfaces stay resident until presentation. Pixel planes are only
        // accessed for software output; no staging texture or CPU readback.
        im.last_is_yuv420 = !im.frame_is_hw &&
            (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P);
        // The YUV→RGB conversion (used only for dump_last_frame_ppm) lives in
        // dump_last_frame_ppm() itself, so the per-frame path stays cheap.
        im.consecutive_errors = 0;
        im.packets_without_frame = 0;
        got_frame = true;
    }

    if (hard_error) {
        ++im.errors;
        if (!got) ++im.consecutive_errors;
        if (im.error_logs++ < 5 || im.error_logs % 100 == 0) {
            char err[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(hard_error, err, sizeof(err));
            LOG_WARN << "H264Decoder: libavcodec error: " << err
                     << " (consecutive=" << im.consecutive_errors
                     << ", total=" << im.errors << ')';
        }
    }

    // Slow GPU decode for a long stretch: the CPU is the better choice.
    if (got && using_hardware() && ms > kSlowGpuDecodeMs) {
        if (++im.slow_streak >= kSlowGpuStreak) {
            fallback_to_software("GPU decoding too slow");
            got_frame = false;   // the picture it produced belongs to the old decoder
            width = height = 0;
            return true;
        }
    } else if (got) {
        im.slow_streak = 0;
    }

    if (!got && (im.consecutive_errors >= kErrorsBeforeResync ||
                 im.packets_without_frame >= kPacketsWithoutFrame)) {
        const auto now = Clock::now();
        while (!im.recent_resyncs.empty() &&
               now - im.recent_resyncs.front() > kEscalateWindow)
            im.recent_resyncs.pop_front();
        im.recent_resyncs.push_back(now);
        const char* why = im.consecutive_errors >= kErrorsBeforeResync
            ? "repeated decode errors" : "decoder produced no pictures";
        if (im.recent_resyncs.size() >= kEscalateCount) {
            im.recent_resyncs.clear();
            if (using_hardware()) {
                fallback_to_software("GPU decoder keeps failing");
            } else {
                LOG_WARN << "decoder: reopening software decoder (" << why << ')';
                im.open(im.codec_id);
                im.begin_wait_for_idr();
                ++im.resyncs;
            }
        } else {
            resync(why);
        }
        return true;
    }
    return !hard_error || got;
}

void H264Decoder::resync(const char* reason) {
    auto& im = *impl_;
    if (!im.ctx) return;
    LOG_WARN << "decoder: resync (" << (reason ? reason : "?")
             << ") — waiting for the next key frame";
    avcodec_flush_buffers(im.ctx);
    im.begin_wait_for_idr();
    ++im.resyncs;
}

bool H264Decoder::using_hardware() const {
    return impl_ && impl_->backend == Impl::HwBackend::D3D11VA &&
           (impl_->frames_out == 0 || impl_->frame_is_hw);
}

bool H264Decoder::hardware_distrusted() {
    if (g_hw_failures.load() < kDistrustFailures) return false;
    const int64_t since = steady_now_ns() - g_last_hw_failure_ns.load();
    return since < std::chrono::duration_cast<std::chrono::nanoseconds>(kDistrustDuration).count();
}

bool H264Decoder::fallback_to_software(const char* reason) {
    auto& im = *impl_;
    if (im.backend != Impl::HwBackend::D3D11VA && !im.hwaccel_requested) return true;
    LOG_WARN << "decoder: switching from GPU to software decoding ("
             << (reason ? reason : "?") << ')';
    ++g_hw_failures;
    g_last_hw_failure_ns.store(steady_now_ns());
    im.hwaccel_requested = false;
    im.hw_pix_fmt = AV_PIX_FMT_NONE;
    // Release surfaces still pinned by the previous decoder output.
    if (im.frame) av_frame_unref(im.frame);
    im.last_w = im.last_h = 0;
    im.frame_is_hw = false;
    if (im.hw_device_d3d11) av_buffer_unref(&im.hw_device_d3d11);
    const bool ok = im.open(im.codec_id);
    im.begin_wait_for_idr();
    ++im.resyncs;
    return ok;
}

H264Decoder::Stats H264Decoder::stats() {
    Stats st;
    if (!impl_) return st;
    auto& im = *impl_;
    st.packets  = im.packets_in;
    st.frames   = im.frames_out;
    st.errors   = im.errors;
    st.skipped  = im.skipped;
    st.resyncs  = im.resyncs;
    st.consecutive_errors = im.consecutive_errors;
    st.last_ms  = im.last_ms;
    st.avg_ms   = im.avg_ms;
    st.max_ms   = im.max_ms;
    im.max_ms   = 0;
    return st;
}

bool H264Decoder::dump_last_frame_ppm(const std::string& path) {
    if (!impl_ || impl_->last_w == 0) return false;
    // Diagnostic dumps must not force readback on the live GPU path.
    if (impl_->frame_is_hw && impl_->frame->format == AV_PIX_FMT_D3D11) return false;

    // Diagnostic conversion is only available for software output.
    AVFrame* rgb_src = impl_->frame;
    if (!rgb_src || rgb_src->width <= 0 || rgb_src->height <= 0) return false;

    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(rgb_src->format);
    bool src_full_range = false;
    switch (src_fmt) {
        case AV_PIX_FMT_YUVJ420P: src_fmt = AV_PIX_FMT_YUV420P; src_full_range = true; break;
        case AV_PIX_FMT_YUVJ422P: src_fmt = AV_PIX_FMT_YUV422P; src_full_range = true; break;
        case AV_PIX_FMT_YUVJ444P: src_fmt = AV_PIX_FMT_YUV444P; src_full_range = true; break;
        default: break;
    }

    impl_->sws = sws_getCachedContext(
        impl_->sws, rgb_src->width, rgb_src->height, src_fmt,
        impl_->last_w, impl_->last_h, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws) return false;

    const int* inv_tbl = nullptr;
    const int* tbl     = nullptr;
    int src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
    sws_getColorspaceDetails(impl_->sws,
        const_cast<int**>(&inv_tbl), &src_range,
        const_cast<int**>(&tbl),     &dst_range,
        &brightness, &contrast, &saturation);
    src_range = src_full_range ? 1 : 0;
    sws_setColorspaceDetails(impl_->sws,
        inv_tbl, src_range, tbl, dst_range,
        brightness, contrast, saturation);

    impl_->last_rgb.assign(
        static_cast<std::size_t>(impl_->last_w) * impl_->last_h * 3, 0);
    uint8_t* dst[1]        = { impl_->last_rgb.data() };
    int      dst_stride[1] = { impl_->last_w * 3 };
    sws_scale(impl_->sws, rgb_src->data, rgb_src->linesize,
              0, rgb_src->height, dst, dst_stride);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", impl_->last_w, impl_->last_h);
    std::fwrite(impl_->last_rgb.data(), 1, impl_->last_rgb.size(), f);
    std::fclose(f);
    LOG_INFO << "H264Decoder: dumped " << impl_->last_w << 'x' << impl_->last_h
             << " frame to " << path;
    return true;
}

bool H264Decoder::last_frame_yuv(const uint8_t*& y, int& y_stride,
                                 const uint8_t*& u, int& u_stride,
                                 const uint8_t*& v, int& v_stride,
                                 int& width, int& height) const {
    // CPU planes are only exposed for software output.
    if (!impl_ || !impl_->last_is_yuv420 || impl_->last_w == 0 ||
        impl_->frame_is_hw || !impl_->frame) return false;
    y = impl_->frame->data[0]; y_stride = impl_->frame->linesize[0];
    u = impl_->frame->data[1]; u_stride = impl_->frame->linesize[1];
    v = impl_->frame->data[2]; v_stride = impl_->frame->linesize[2];
    width  = impl_->last_w;
    height = impl_->last_h;
    return y && u && v;
}

const AVFrame* H264Decoder::current_avframe() const {
    if (!impl_ || impl_->last_w == 0) return nullptr;
    // Return the original CPU or GPU output, without transferring pixels.
    return impl_->frame;
}

uint64_t H264Decoder::frames_decoded() const { return impl_ ? impl_->frames_out : 0; }

bool H264Decoder::is_hevc() const {
    return impl_ && impl_->codec_id == AV_CODEC_ID_HEVC;
}

} // namespace ap::video
