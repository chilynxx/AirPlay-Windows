#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
struct AVFrame;
struct AVBufferRef;
}

namespace ap::video {

// Minimal FFmpeg-backed H.264 decoder sized for the mirror stream path.
//
// Feed it:
//   1. `set_parameter_sets_from_avcc(avcc_bytes)` once per resolution change
//      (received as SPS_PPS mirror frames). The avcC blob contains the SPS
//      and PPS NAL units; we extract both and keep them in Annex-B form so
//      they can be prepended to every IDR packet we dispatch.
//   2. `decode(annexb_nal_bytes, is_idr)` for every decrypted mirror frame.
//      For IDR packets we prepend the stored SPS+PPS automatically
//      (libavcodec needs them to initialise the reference frames).
//
// Decoded frames are kept internally; use `last_frame_size()` to know the
// resolution and `dump_last_frame_ppm()` to write the most recent YUV →
// RGB-converted frame to disk as proof the pipeline works.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&)            = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Initialise with an H.264 decoder (the historical path for the
    // AirPlay mirror stream). If iOS later sends an hvcC blob in a
    // SPS_PPS frame — because we now advertise bit 42 / HEVC — the
    // decoder transparently switches to H.265 via reinit_for_hevc().
    //
    // hwaccel=true uses the renderer's shared D3D11VA device. Frames stay
    // in GPU memory. Without a render device, decoding uses software.
    bool init(bool hwaccel = false, AVBufferRef* render_device = nullptr);

    // Parse SPS/PPS out of an avcC (H.264) or hvcC (HEVC) blob iOS
    // sent as an SPS_PPS mirror frame, caching them as Annex-B bytes
    // for later prepend to IDR packets. Switches the internal decoder
    // to the matching codec if needed. Returns false on malformed
    // input (both parsers tried).
    bool set_parameter_sets_from_avcc(const uint8_t* avcc, std::size_t size);

    // Returns true iff the packet was accepted by avcodec. `got_frame` is set
    // when a complete picture comes out; width/height describe that frame.
    bool decode(const uint8_t* nal_data, std::size_t nal_size, bool is_idr,
                bool& got_frame, int& width, int& height);

    bool dump_last_frame_ppm(const std::string& path);

    // Raw YUV420P pointers to the most recently decoded frame. Only valid
    // until the next decode() call. Returns false when nothing is available
    // yet or the last frame was not 4:2:0.
    bool last_frame_yuv(const uint8_t*& y, int& y_stride,
                        const uint8_t*& u, int& u_stride,
                        const uint8_t*& v, int& v_stride,
                        int& width, int& height) const;

    // The most recently decoded frame:
    //   - software path: the libavcodec output (typically
    //     AV_PIX_FMT_YUV420P / YUVJ420P)
    //   - hwaccel path: a resident AV_PIX_FMT_D3D11 decoder surface
    // Only valid until the next decode() call. Returned as a
    // const pointer so the renderer can av_frame_ref / clone
    // without touching decoder state. Returns nullptr until a
    // first decode produces a frame.
    const AVFrame* current_avframe() const;

    // Number of frames successfully decoded since init().
    uint64_t frames_decoded() const;

    // True after set_parameter_sets_from_avcc() has reinitialised the
    // backend with the HEVC codec (because iOS sent an hvcC config).
    // Lets the mirror_listener pick the right NAL-header layout when
    // logging decrypted IDR / non-IDR frames.
    bool is_hevc() const;

    // ---- health and recovery ----------------------------------------------

    // True while the decoder is producing D3D11 hardware surfaces.
    bool using_hardware() const;

    // Leave the GPU path and continue in software. The cached parameter sets
    // are kept, so the next IDR decodes normally. Safe to call when already
    // running in software (returns true and does nothing). Also records the
    // failure so later sessions in this process do not retry a broken GPU.
    bool fallback_to_software(const char* reason);

    // True once the GPU path failed in this process (see above). New decoders
    // then start in software even when GPU decoding is enabled in settings.
    static bool hardware_distrusted();

    // Flush libavcodec's reference frames and skip inter frames until the next
    // IDR (for at most ~1.5 s, so a sender that rarely sends IDRs cannot leave
    // the picture frozen). Called automatically after repeated errors.
    void resync(const char* reason);

    struct Stats {
        uint64_t packets          = 0;   // packets handed to libavcodec
        uint64_t frames           = 0;   // pictures produced
        uint64_t errors           = 0;   // failed send/receive calls
        uint64_t skipped          = 0;   // inter frames skipped while waiting for IDR
        uint64_t resyncs          = 0;
        int      consecutive_errors = 0;
        double   last_ms          = 0;   // duration of the latest decode() call
        double   avg_ms           = 0;   // smoothed
        double   max_ms           = 0;   // since the previous stats() call
    };
    // Returns a snapshot and resets the max_ms window.
    Stats stats();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::video
