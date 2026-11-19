#include "video/video_renderer.h"
#include "airplay/live_settings.h"
#include "airplay/receiver_name.h"
#include "app_info.h"
#include "audio/master_volume.h"
#include "i18n/localization.h"
#include "log.h"
#include "platform/desktop_controls.h"
#include "platform/firewall.h"
#include "platform/single_instance.h"
#include "platform/thread_tuning.h"
#include "usb/usb_supervisor.h"
#include "video/cover_art.h"
#include "video/gpu_video.h"
#include "video/receiver_ui.h"
#include "video/video_recorder.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
    #include <dwmapi.h>
    #include <shobjidl.h>
    #ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
        #define DWMWA_USE_IMMERSIVE_DARK_MODE 20
    #endif
    #ifndef DWMWA_WINDOW_CORNER_PREFERENCE
        #define DWMWA_WINDOW_CORNER_PREFERENCE 33
    #endif
    #ifndef DWMWCP_ROUND
        #define DWMWCP_ROUND 2
    #endif
    #ifndef DWMWA_SYSTEMBACKDROP_TYPE
        #define DWMWA_SYSTEMBACKDROP_TYPE 38
    #endif
    #ifndef DWMSBT_MAINWINDOW
        #define DWMSBT_MAINWINDOW 2
    #endif
#endif

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

constexpr int kDefaultWinWidth  = 1240;
constexpr int kDefaultWinHeight = 820;
constexpr int kSidebarWidth     = 200;
constexpr int kOptionsWidth     = 400;
constexpr int kStatusLineHeight = 24;
constexpr int kToolbarHeight    = 46;

#if defined(_WIN32)
void configure_win11_window_chrome(SDL_Window* w, bool dark_mode) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return;
    HWND hwnd = info.info.win.window;
    BOOL use_dark = dark_mode ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                            &use_dark, sizeof(use_dark));
    int corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                            &corner, sizeof(corner));
    int backdrop = DWMSBT_MAINWINDOW;
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                            &backdrop, sizeof(backdrop));
    HICON hicon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (hicon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hicon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hicon));
    }
}

bool browse_recording_folder(SDL_Window* w, const std::string& initial,
                             std::string& selected) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return false;

    const HRESULT init_hr = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(init_hr);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr) && dialog) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
        const std::wstring title_w = ap::i18n::tr_w(ap::i18n::Str::SettingsBrowseDialogTitle);
        dialog->SetTitle(title_w.c_str());

        try {
            const std::wstring initial_w =
                std::filesystem::u8path(initial).wstring();
            IShellItem* initial_item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initial_w.c_str(), nullptr, IID_PPV_ARGS(&initial_item)))) {
                dialog->SetFolder(initial_item);
                initial_item->Release();
            }
        } catch (const std::exception&) {}

        hr = dialog->Show(info.info.win.window);
        if (SUCCEEDED(hr)) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(result->GetDisplayName(
                        SIGDN_FILESYSPATH, &path_w)) && path_w) {
                    selected = std::filesystem::path(path_w).u8string();
                    CoTaskMemFree(path_w);
                }
                result->Release();
            }
        }
        dialog->Release();
    }
    if (should_uninitialize) CoUninitialize();
    return !selected.empty();
}
#else
void configure_win11_window_chrome(SDL_Window*, bool) {}
#endif

void apply_overlay_theme(bool dark_mode) {
    if (dark_mode) ImGui::StyleColorsDark();
    else           ImGui::StyleColorsLight();
    const auto pal = receiver_ui::get_palette(dark_mode);
    auto& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildRounding     = 12.0f;
    s.FrameRounding     = 9.0f;
    s.PopupRounding     = 12.0f;
    s.GrabRounding      = 9.0f;
    s.WindowPadding     = ImVec2(20, 20);
    s.FrameBorderSize   = 1.0f;
    s.ScrollbarSize     = 10.0f;
    s.FramePadding      = ImVec2(12, 8);
    s.ItemSpacing       = ImVec2(10, 12);

    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImGui::ColorConvertU32ToFloat4(pal.bg_window);
    c[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]          = ImGui::ColorConvertU32ToFloat4(pal.bg_card);
    c[ImGuiCol_Text]             = ImGui::ColorConvertU32ToFloat4(pal.text_ink);
    c[ImGuiCol_TextDisabled]     = ImGui::ColorConvertU32ToFloat4(pal.text_muted);
    c[ImGuiCol_Border]           = ImGui::ColorConvertU32ToFloat4(pal.border_line);
    c[ImGuiCol_Separator]        = ImGui::ColorConvertU32ToFloat4(pal.border_line);
    c[ImGuiCol_FrameBg]          = ImGui::ColorConvertU32ToFloat4(pal.bg_pill_idle);
    c[ImGuiCol_FrameBgHovered]   = ImGui::ColorConvertU32ToFloat4(pal.bg_card_hover);
    c[ImGuiCol_FrameBgActive]    = ImGui::ColorConvertU32ToFloat4(pal.accent_blue_soft);
    c[ImGuiCol_Button]           = ImGui::ColorConvertU32ToFloat4(pal.bg_card);
    c[ImGuiCol_ButtonHovered]    = ImGui::ColorConvertU32ToFloat4(pal.bg_card_hover);
    c[ImGuiCol_ButtonActive]     = ImGui::ColorConvertU32ToFloat4(pal.accent_blue_soft);
    c[ImGuiCol_Header]           = ImGui::ColorConvertU32ToFloat4(pal.bg_card);
    c[ImGuiCol_HeaderHovered]    = ImGui::ColorConvertU32ToFloat4(pal.bg_card_hover);
    c[ImGuiCol_HeaderActive]     = ImGui::ColorConvertU32ToFloat4(pal.accent_blue_soft);
    c[ImGuiCol_SliderGrab]       = ImGui::ColorConvertU32ToFloat4(pal.accent_blue);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_CheckMark]        = ImGui::ColorConvertU32ToFloat4(pal.accent_blue);
    c[ImGuiCol_PlotHistogram]    = ImGui::ColorConvertU32ToFloat4(pal.accent_blue);
}

void copy_plane(std::vector<unsigned char>& dst,
                const uint8_t* src, int stride,
                int plane_w, int plane_h) {
    dst.resize(static_cast<std::size_t>(plane_w) * plane_h);
    for (int row = 0; row < plane_h; ++row) {
        std::memcpy(dst.data() + static_cast<std::size_t>(row) * plane_w,
                    src + static_cast<std::size_t>(row) * stride,
                    static_cast<std::size_t>(plane_w));
    }
}

struct UiState {
    bool chromeless = false;
    bool settings_open = false;
    bool diagnostics = false;
    int  page = 0;
    bool was_connected = false;
};

SDL_HitTestResult SDLCALL hit_test_cb(SDL_Window* w,
                                      const SDL_Point* p,
                                      void* data) {
    auto* ui = reinterpret_cast<UiState*>(data);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(w, &win_w, &win_h);
    const Uint32 flags = SDL_GetWindowFlags(w);
    if (flags & SDL_WINDOW_FULLSCREEN) return SDL_HITTEST_NORMAL;
    const int B = ui && ui->chromeless ? 10 : 6;
    const bool resizable = !(flags & SDL_WINDOW_MAXIMIZED);
    const bool top    = resizable && p->y < B;
    const bool bottom = resizable && p->y >= win_h - B;
    const bool left   = resizable && p->x < B;
    const bool right  = resizable && p->x >= win_w - B;
    if (top    && left)  return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top    && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)             return SDL_HITTEST_RESIZE_TOP;
    if (bottom)          return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)            return SDL_HITTEST_RESIZE_LEFT;
    if (right)           return SDL_HITTEST_RESIZE_RIGHT;
    // Leave room on the right of the titlebar for the language pill, theme toggle, and window buttons
    if (ui && (ui->chromeless || (p->y < kToolbarHeight && p->x < win_w - 260)))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

std::vector<TTF_Font*> open_font_chain(int pt_size) {
    static const std::array<const char*, 12> candidates = {{
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",
        "C:\\Windows\\Fonts\\seguisym.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    }};
    std::vector<TTF_Font*> fonts;
    for (const char* path : candidates) {
        if (!path) continue;
        TTF_Font* f = TTF_OpenFont(path, pt_size);
        if (f) {
            fonts.push_back(f);
            LOG_INFO << "VideoRenderer loaded font " << path << " @" << pt_size << "pt";
        }
    }
    return fonts;
}

uint32_t utf8_next(const std::string& s, std::size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { ++i; return c0; }
    uint32_t cp = 0;
    int      n  = 0;
    if      ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; n = 1; }
    else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; n = 2; }
    else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; n = 3; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < n; ++k) {
        if (i >= s.size()) return 0xFFFD;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xc0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (c & 0x3f);
        ++i;
    }
    return cp;
}

TTF_Font* pick_font_for(uint32_t cp, const std::vector<TTF_Font*>& fonts) {
    for (TTF_Font* f : fonts) {
        if (TTF_GlyphIsProvided32(f, cp)) return f;
    }
    return fonts.empty() ? nullptr : fonts[0];
}

SDL_Surface* render_multi_font(const std::vector<TTF_Font*>& fonts,
                               const std::string& utf8,
                               SDL_Color color) {
    if (fonts.empty() || utf8.empty()) return nullptr;
    struct Run { TTF_Font* font; std::string text; };
    std::vector<Run> runs;

    TTF_Font*   cur_font = nullptr;
    std::string cur_text;
    for (std::size_t i = 0; i < utf8.size(); ) {
        std::size_t start = i;
        uint32_t    cp    = utf8_next(utf8, i);
        TTF_Font*   f     = pick_font_for(cp, fonts);
        if (f != cur_font) {
            if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});
            cur_text.clear();
            cur_font = f;
        }
        cur_text.append(utf8, start, i - start);
    }
    if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});

    if (runs.size() == 1 && runs[0].font) {
        return TTF_RenderUTF8_Blended(runs[0].font, runs[0].text.c_str(), color);
    }

    std::vector<SDL_Surface*> surfs;
    int total_w = 0, max_h = 0;
    for (const auto& r : runs) {
        if (!r.font) { surfs.push_back(nullptr); continue; }
        SDL_Surface* s = TTF_RenderUTF8_Blended(r.font, r.text.c_str(), color);
        surfs.push_back(s);
        if (s) { total_w += s->w; max_h = std::max(max_h, s->h); }
    }
    if (total_w == 0) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }

    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(
        0, total_w, max_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!out) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }
    SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_NONE);

    int x = 0;
    for (SDL_Surface* s : surfs) {
        if (!s) continue;
        SDL_Rect dst{ x, max_h - s->h, s->w, s->h };
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(s, nullptr, out, &dst);
        x += s->w;
        SDL_FreeSurface(s);
    }
    return out;
}

SDL_Rect fit_inside(int src_w, int src_h, int win_w, int win_h) {
    SDL_Rect r{0, 0, win_w, win_h};
    if (src_w <= 0 || src_h <= 0) return r;
    const double src_ar = static_cast<double>(src_w) / src_h;
    const double win_ar = static_cast<double>(win_w) / win_h;
    if (src_ar > win_ar) {
        r.w = win_w;
        r.h = static_cast<int>(win_w / src_ar);
        r.x = 0;
        r.y = (win_h - r.h) / 2;
    } else {
        r.h = win_h;
        r.w = static_cast<int>(win_h * src_ar);
        r.x = (win_w - r.w) / 2;
        r.y = 0;
    }
    return r;
}

// Save an instant screenshot of the active renderer viewport to `folder_utf8`.
std::string capture_renderer_screenshot(SDL_Renderer* renderer,
                                        const SDL_Rect& area,
                                        const std::string& folder_utf8) {
    if (!renderer || area.w <= 0 || area.h <= 0) return {};
    std::error_code ec;
    std::filesystem::path dir = folder_utf8.empty()
        ? std::filesystem::u8path(VideoRecorder::default_directory())
        : std::filesystem::u8path(folder_utf8);
    std::filesystem::create_directories(dir, ec);

    const auto now_sys = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now_sys);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &tt);
#else
    localtime_r(&tt, &tm_local);
#endif
    char fname[64];
    std::snprintf(fname, sizeof(fname),
                  "WinAirPlay_%04d%02d%02d_%02d%02d%02d.bmp",
                  tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                  tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    const auto out_path = dir / fname;

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
        0, area.w, area.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return {};
    if (SDL_RenderReadPixels(renderer, &area, SDL_PIXELFORMAT_ARGB8888,
                             surf->pixels, surf->pitch) != 0) {
        SDL_FreeSurface(surf);
        return {};
    }
    const std::string out_u8 = out_path.u8string();
    const int saved = SDL_SaveBMP(surf, out_u8.c_str());
    SDL_FreeSurface(surf);
    if (saved != 0) return {};
    LOG_NOTICE << "Saved screenshot: " << out_u8;
    return out_u8;
}

} // namespace

VideoRenderer::VideoRenderer() : recorder_(std::make_unique<VideoRecorder>()) {}
VideoRenderer::~VideoRenderer() {
    stop();
    av_buffer_unref(&gpu_device_);
}

AVBufferRef* VideoRenderer::gpu_device_ref() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait_for(lock, std::chrono::seconds(5), [this] {
        return gpu_ready_ || !running_.load();
    });
    return running_.load() && gpu_device_ ? av_buffer_ref(gpu_device_) : nullptr;
}

bool VideoRenderer::start(const std::string& title) {
    running_ = true;
    thread_  = std::thread(&VideoRenderer::run, this, title);
    return true;
}

void VideoRenderer::stop() {
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (recorder_) recorder_->stop();
}

void VideoRenderer::push_cover_art(const uint8_t* jpeg, std::size_t size) {
    if (!jpeg || size == 0) return;
    std::lock_guard<std::mutex> lock(cover_mtx_);
    cover_jpeg_.assign(jpeg, jpeg + size);
    cover_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_progress(uint32_t elapsed_ms, uint32_t total_ms) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    progress_elapsed_ms_.store(elapsed_ms, std::memory_order_relaxed);
    progress_total_ms_.store(total_ms,     std::memory_order_relaxed);
    progress_pushed_at_ns_.store(ns,       std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::push_playback_rate(float rate) {
    const bool new_playing = rate > 0.5f;
    const bool old_playing = playing_.exchange(new_playing, std::memory_order_relaxed);
    if (old_playing != new_playing) {
        LOG_INFO << "VideoRenderer playback state: "
                 << (new_playing ? "PLAYING" : "PAUSED");
    }
    cv_.notify_one();
}

void VideoRenderer::note_flush() {
    constexpr int64_t grace_ms = 500;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    flush_grace_until_ns_.store(now_ns + grace_ms * 1'000'000,
                                 std::memory_order_relaxed);
}

bool VideoRenderer::in_flush_grace() const {
    const int64_t until = flush_grace_until_ns_.load(std::memory_order_relaxed);
    if (until == 0) return false;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now_ns < until;
}

void VideoRenderer::clear_session() {
    LOG_INFO << "VideoRenderer clear_session: reset cover / metadata / progress / video frame";
    audio_control_available_.store(false);
    audio_command_pending_.store(false);
    audio_command_failed_.store(false);
    reconnecting_overlay_.store(false);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        has_frame_      = false;
        frame_is_nv12_  = false;
        frame_w_        = 0;
        frame_h_        = 0;
        slot_avframe_   = DecodedFrame();
        has_avframe_    = false;
    }
    {
        std::lock_guard<std::mutex> lock(meta_mtx_);
        meta_title_.clear();
        meta_artist_.clear();
        meta_album_.clear();
        meta_dirty_ = true;
    }
    progress_elapsed_ms_.store(0, std::memory_order_relaxed);
    progress_total_ms_.store(0,   std::memory_order_relaxed);
    progress_pushed_at_ns_.store(0, std::memory_order_relaxed);
    playing_.store(true, std::memory_order_relaxed);
    clear_cover_requested_.store(true, std::memory_order_relaxed);
    frames_total_.store(0, std::memory_order_relaxed);
    video_fps_ema_.store(0.0f, std::memory_order_relaxed);
    last_push_ns_.store(0, std::memory_order_relaxed);
    payload_bytes_total_.store(0, std::memory_order_relaxed);
    payload_mbps_ema_.store(0.0f, std::memory_order_relaxed);
    last_payload_ns_.store(0, std::memory_order_relaxed);
    pipeline_latency_ms_ema_.store(0.0f, std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::set_live_settings(ap::airplay::LiveSettings* live) {
    live_settings_ = live;
}

DecodedFrame::~DecodedFrame() {
    if (frame) av_frame_free(&frame);
}
DecodedFrame::DecodedFrame(DecodedFrame&& o) noexcept : frame(o.frame) {
    o.frame = nullptr;
}
DecodedFrame& DecodedFrame::operator=(DecodedFrame&& o) noexcept {
    if (this != &o) {
        if (frame) av_frame_free(&frame);
        frame = o.frame;
        o.frame = nullptr;
    }
    return *this;
}

void VideoRenderer::push_avframe(const AVFrame* src, int64_t origin_ns) {
    if (!src) return;
    if (recorder_) recorder_->submit(src);
    AVFrame* clone = av_frame_clone(src);
    if (!clone) return;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(clone->width,  std::memory_order_relaxed);
    video_h_seen_.store(clone->height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);
    reconnecting_overlay_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (has_avframe_) frames_replaced_.fetch_add(1, std::memory_order_relaxed);
        slot_avframe_   = DecodedFrame{clone};
        frame_w_        = clone->width;
        frame_h_        = clone->height;
        has_frame_      = false;
        frame_is_nv12_  = false;
        has_avframe_    = true;
        frame_origin_ns_= origin_ns;
        cv_.notify_one();
    }
}

void VideoRenderer::push_audio_pcm(const int16_t* samples, int sample_count,
                                   int sample_rate, int channels) {
    if (recorder_)
        recorder_->submit_audio(samples, sample_count, sample_rate, channels);
}

void VideoRenderer::record_payload_bytes(std::size_t n) {
    payload_bytes_total_.fetch_add(static_cast<uint64_t>(n),
                                   std::memory_order_relaxed);

    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_payload_ns_.exchange(now_ns,
                                                      std::memory_order_relaxed);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_mbps =
                static_cast<float>((static_cast<double>(n) * 8.0) /
                                   (dt_s * 1e6));
            const float prev_ema =
                payload_mbps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_mbps
                                  : prev_ema * 0.85f + inst_mbps * 0.15f;
            payload_mbps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
}

void VideoRenderer::set_active_device(DeviceInfo info) {
    std::lock_guard<std::mutex> lock(device_mtx_);
    device_     = std::move(info);
    has_device_ = true;
}

void VideoRenderer::clear_active_device() {
    std::lock_guard<std::mutex> lock(device_mtx_);
    has_device_ = false;
    device_     = {};
}

bool VideoRenderer::clear_session_if_active(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(device_mtx_);
    if (!has_device_ || device_.session_id != session_id) return false;
    clear_session();
    has_device_ = false;
    device_ = {};
    LOG_INFO << "Connection ended: cleared active session=" << session_id;
    return true;
}

void VideoRenderer::set_audio_control_handler(
        std::function<void(ap::airplay::MediaCommand, uint32_t)> handler) {
    audio_control_handler_ = std::move(handler);
}

void VideoRenderer::set_disconnect_handler(std::function<void()> h) {
    disconnect_handler_ = std::move(h);
}

bool VideoRenderer::active_device_snapshot(DeviceInfo& out) const {
    std::lock_guard<std::mutex> lock(device_mtx_);
    if (!has_device_) return false;
    out = device_;
    return true;
}

void VideoRenderer::set_idle_info(const std::string& name,
                                  const std::string& ip) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    idle_name_  = name;
    idle_ip_    = ip;
    idle_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_metadata(const std::string& title,
                                  const std::string& artist,
                                  const std::string& album) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    if (meta_title_ == title && meta_artist_ == artist && meta_album_ == album) {
        return;
    }
    meta_title_  = title;
    meta_artist_ = artist;
    meta_album_  = album;
    meta_dirty_  = true;
    cv_.notify_one();
}

void VideoRenderer::push_frame(const uint8_t* y, int y_stride,
                               const uint8_t* u, int u_stride,
                               const uint8_t* v, int v_stride,
                               int width, int height,
                               int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    if (recorder_) recorder_->submit_i420(y, y_stride, u, u_stride,
                                          v, v_stride, width, height);
    const int c_w = width  / 2;
    const int c_h = height / 2;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_, y, y_stride, width, height);
    copy_plane(u_buf_, u, u_stride, c_w,   c_h);
    copy_plane(v_buf_, v, v_stride, c_w,   c_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = false;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::push_frame_nv12(const uint8_t* y,  int y_stride,
                                    const uint8_t* uv, int uv_stride,
                                    int width, int height,
                                    int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    if (recorder_) recorder_->submit_nv12(y, y_stride, uv, uv_stride,
                                          width, height);
    const int uv_h = height / 2;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_,  y,  y_stride,  width, height);
    copy_plane(uv_buf_, uv, uv_stride, width, uv_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = true;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::run(const std::string& title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERROR << "SDL_Init failed: " << SDL_GetError();
        running_ = false;
        return;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
#if defined(_WIN32)
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
#endif

    auto preferences = live_settings_ ? live_settings_->ui_preferences() : ap::UiPreferences{};
    ap::i18n::LocalizationManager::instance().set_language(
        static_cast<ap::i18n::Language>(preferences.language));

    int initial_x = SDL_WINDOWPOS_CENTERED, initial_y = SDL_WINDOWPOS_CENTERED;
    int initial_w = kDefaultWinWidth, initial_h = kDefaultWinHeight;
    if (preferences.window_placement_valid) {
        initial_w = preferences.window_width;
        initial_h = preferences.window_height;
        SDL_Rect available{};
        bool found = false;
        for (int display = 0; display < SDL_GetNumVideoDisplays(); ++display) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayUsableBounds(display, &bounds) != 0) continue;
            if (preferences.window_y >= bounds.y &&
                preferences.window_y + kToolbarHeight <= bounds.y + bounds.h &&
                preferences.window_x + 100 <= bounds.x + bounds.w &&
                preferences.window_x + initial_w - 100 >= bounds.x) {
                available = bounds; found = true; break;
            }
        }
        if (found || SDL_GetDisplayUsableBounds(0, &available) == 0) {
            initial_w = std::min(initial_w, available.w);
            initial_h = std::min(initial_h, available.h);
            initial_x = found ? std::clamp(preferences.window_x, available.x, available.x + available.w - initial_w)
                              : available.x + (available.w - initial_w) / 2;
            initial_y = found ? std::clamp(preferences.window_y, available.y, available.y + available.h - initial_h)
                              : available.y + (available.h - initial_h) / 2;
        }
    }

    SDL_Window* window = SDL_CreateWindow(
        title.c_str(), initial_x, initial_y, initial_w, initial_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
    const bool vsync_initial = live_settings_
        ? live_settings_->vsync_enabled.load(std::memory_order_relaxed)
        : true;
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (vsync_initial) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
#if defined(_WIN32)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
#endif
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, renderer_flags)
        : nullptr;
    bool vsync_active = vsync_initial;
    if (!window || !renderer) {
        LOG_ERROR << "SDL window/renderer create failed: " << SDL_GetError();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window)   SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        running_ = false;
        return;
    }
    LOG_INFO << "WinAirPlay VideoRenderer window up (" << initial_w
             << 'x' << initial_h << ", resizable)";
    SDL_SetWindowMinimumSize(window, 980, 720);

    GpuVideo gpu_video;
    ap::platform::DesktopControls desktop_controls;
#if defined(_WIN32)
    SDL_SysWMinfo instance_window_info{};
    SDL_VERSION(&instance_window_info.version);
    if (SDL_GetWindowWMInfo(window, &instance_window_info)) {
        SetPropW(instance_window_info.info.win.window,
                 ap::platform::kInstanceWindowProperty, reinterpret_cast<HANDLE>(1));
        desktop_controls.start(
            instance_window_info.info.win.window,
            [this, window](bool show) {
                if (show) request_show();
                else SDL_HideWindow(window);
            },
            [this]() {
                closed_.store(true);
                running_.store(false);
            },
            [this](ap::airplay::MediaCommand cmd) {
                if (audio_control_handler_) audio_control_handler_(cmd, 0);
            });
    }
#endif
    gpu_video.init(renderer);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        av_buffer_unref(&gpu_device_);
        gpu_device_ = gpu_video.device_ref();
        gpu_ready_ = true;
        cv_.notify_all();
    }

    configure_win11_window_chrome(window, preferences.dark_mode);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_overlay_theme(preferences.dark_mode);
    bool applied_dark_mode = preferences.dark_mode;

    // Comprehensive Unicode glyph range for English + Full Vietnamese diacritics + UI symbols
    static const ImWchar kFullVietnameseGlyphRanges[] = {
        0x0020, 0x024F, // Basic Latin + Latin-1 Supplement + Latin Extended-A + Latin Extended-B (Ă, ă, Đ, đ, Ơ, ơ, Ư, ư)
        0x0300, 0x036F, // Combining Diacritical Marks
        0x1E00, 0x1EFF, // Latin Extended Additional (All precomposed Vietnamese vowels & tone marks)
        0x2000, 0x206F, // General Punctuation (·, —, …, •)
        0x2190, 0x21FF, // Arrows
        0,
    };

    ImFont* heading_font  = nullptr;
    ImFont* bold_font     = nullptr;
    ImFont* small_font    = nullptr;
    ImFont* page_font     = nullptr;
    ImFont* receiver_font = nullptr;
#if defined(_WIN32)
    if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/segoeui.ttf", 17.5f, nullptr, kFullVietnameseGlyphRanges);
        heading_font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/seguisb.ttf", 38.0f, nullptr, kFullVietnameseGlyphRanges);
        bold_font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/seguisb.ttf", 17.5f, nullptr, kFullVietnameseGlyphRanges);
        page_font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/seguisb.ttf", 25.0f, nullptr, kFullVietnameseGlyphRanges);
        small_font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/segoeui.ttf", 15.0f, nullptr, kFullVietnameseGlyphRanges);
        receiver_font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/seguisb.ttf", 22.0f, nullptr, kFullVietnameseGlyphRanges);
    }
#endif
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();
    if (!bold_font)      bold_font      = io.FontDefault;
    if (!small_font)     small_font     = io.FontDefault;
    if (!heading_font)   heading_font   = io.FontDefault;
    if (!page_font)      page_font      = bold_font;
    if (!receiver_font)  receiver_font  = bold_font;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SDL_Texture* video_tex = nullptr;
    int          video_tex_w = 0, video_tex_h = 0;
    Uint32       video_tex_fmt = 0;

    SDL_Texture* cover_tex = nullptr;
    int          cover_tex_w = 0, cover_tex_h = 0;

    std::vector<TTF_Font*> fonts_big;
    std::vector<TTF_Font*> fonts_small;
    if (TTF_Init() == 0) {
        fonts_big   = open_font_chain(32);
        fonts_small = open_font_chain(20);
        if (TTF_Font* semibold = TTF_OpenFont("C:/Windows/Fonts/seguisb.ttf", 32)) {
            if (!fonts_big.empty()) { TTF_CloseFont(fonts_big.front()); fonts_big.front() = semibold; }
            else fonts_big.push_back(semibold);
        }
    } else {
        LOG_WARN << "TTF_Init failed: " << TTF_GetError();
    }
    SDL_Texture* title_tex     = nullptr; int title_w     = 0, title_h     = 0;
    SDL_Texture* artist_tex    = nullptr; int artist_w    = 0, artist_h    = 0;
    SDL_Texture* album_tex     = nullptr; int album_w     = 0, album_h     = 0;
    SDL_Texture* idle_name_tex = nullptr; int idle_name_w = 0, idle_name_h = 0;
    SDL_Texture* idle_ip_tex   = nullptr; int idle_ip_w   = 0, idle_ip_h   = 0;
    SDL_Texture* idle_msg_tex  = nullptr; int idle_msg_w  = 0, idle_msg_h  = 0;

    auto make_text = [&](const std::vector<TTF_Font*>& fonts,
                         const std::string& s, SDL_Color c,
                         SDL_Texture*& tex, int& w, int& h) {
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        w = h = 0;
        if (fonts.empty() || s.empty()) return;
        SDL_Surface* surf = render_multi_font(fonts, s, c);
        if (!surf) return;
        tex = SDL_CreateTextureFromSurface(renderer, surf);
        w = surf->w; h = surf->h;
        SDL_FreeSurface(surf);
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    };

    auto last_video_frame_time = std::chrono::steady_clock::now()
                                 - std::chrono::seconds(10);

    bool  vsync_relief = false;
    int   slow_present_streak = 0;
    uint64_t slow_present_total = 0;
    std::chrono::steady_clock::time_point vsync_relief_since{};
    bool  keep_awake_on = false;

    UiState ui;
    ui.diagnostics = preferences.diagnostics;
    if (live_settings_)
        ui.chromeless = live_settings_->chromeless.load(std::memory_order_relaxed);
    if (SDL_SetWindowHitTest(window, hit_test_cb, &ui) != 0)
        LOG_WARN << "Window resize hit-test unavailable: " << SDL_GetError();
    if (ui.chromeless) SDL_SetWindowMinimumSize(window, 160, 120);

    std::array<char, 1024> recording_dir{};
    std::array<char, 51> receiver_name_buf{};
    std::array<char, 128> android_ip_buf{};
    if (live_settings_)
        std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s",
                      live_settings_->receiver_name().c_str());
    std::snprintf(android_ip_buf.data(), android_ip_buf.size(), "%s",
                  preferences.android_ip.empty() ? "192.168.1.50:5555" : preferences.android_ip.c_str());

    VideoRecorder::Options recording_options;
    recording_options.codec = static_cast<VideoRecorder::VideoCodec>(preferences.recording_codec);
    recording_options.encoder = static_cast<VideoRecorder::EncoderMode>(preferences.recording_encoder);
    recording_options.bitrate_mbps = preferences.recording_bitrate;
    const std::string default_recording_dir = VideoRecorder::default_directory();
    std::snprintf(recording_dir.data(), recording_dir.size(), "%s",
                  preferences.recording_directory.empty() ? default_recording_dir.c_str() : preferences.recording_directory.c_str());

    auto toggle_recording = [&]() {
        if (!recorder_) return;
        if (recorder_->status().recording) recorder_->stop();
        else recorder_->start(recording_dir.data(), recording_options);
    };

    bool screenshot_requested = false;
    double screenshot_toast_until = 0.0;
    std::string last_screenshot_path;

    bool fullscreen = live_settings_
        ? live_settings_->fullscreen.load(std::memory_order_relaxed) : false;
    auto toggle_fullscreen = [&]() {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(window,
            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(SDL_ENABLE);
        if (live_settings_)
            live_settings_->fullscreen.store(fullscreen, std::memory_order_relaxed);
    };
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else if (!ui.chromeless && preferences.window_maximized) {
        SDL_MaximizeWindow(window);
    }

    auto publish_preferences = [&]() {
        auto current = preferences;
        current.diagnostics = ui.diagnostics;
        current.recording_directory = recording_dir.data();
        current.recording_codec = static_cast<int>(recording_options.codec);
        current.recording_encoder = static_cast<int>(recording_options.encoder);
        current.recording_bitrate = recording_options.bitrate_mbps;
        current.language = static_cast<int>(ap::i18n::current_language());
        current.android_ip = android_ip_buf.data();
        const Uint32 flags = SDL_GetWindowFlags(window);
        if (!fullscreen && !ui.chromeless && !(flags & SDL_WINDOW_MINIMIZED)) {
            current.window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            if (!current.window_maximized) {
                SDL_GetWindowPosition(window, &current.window_x, &current.window_y);
                SDL_GetWindowSize(window, &current.window_width, &current.window_height);
                current.window_placement_valid = true;
            }
        }
        if (!(current == preferences)) {
            preferences = current;
            if (live_settings_) live_settings_->set_ui_preferences(current);
        }
    };

    bool prev_chromeless    = false;
    int  prev_video_w       = 0;
    int  prev_video_h       = 0;
    int  saved_window_w     = 0;
    int  saved_window_h     = 0;
    bool saved_window_maximized = false;
    bool custom_resolution  = false;
    double settings_reset_notice_until = 0.0;

    // Auto-hiding Live Actions Control Dock timer
    auto last_mouse_move_time = std::chrono::steady_clock::now();
    int  prev_mouse_x = -1, prev_mouse_y = -1;

    auto reset_all_settings = [&]() {
        if (!live_settings_) return;
        const ap::PersistentSettings defaults;
        int mirror_w = 2560, mirror_h = 1440;
        SDL_DisplayMode display{};
        if (SDL_GetCurrentDisplayMode(0, &display) == 0 &&
            display.w >= 320 && display.h >= 240 && display.w <= 7680 && display.h <= 4320) {
            mirror_w = display.w;
            mirror_h = display.h;
        }
        live_settings_->set_receiver_name(defaults.receiver_name);
        live_settings_->mirror_width.store(mirror_w);
        live_settings_->mirror_height.store(mirror_h);
        live_settings_->hevc_enabled.store(defaults.hevc_enabled);
        live_settings_->max_fps.store(defaults.max_fps);
        live_settings_->refresh_rate.store(defaults.refresh_rate);
        live_settings_->mirror_hwaccel.store(defaults.mirror_hwaccel);
        live_settings_->vsync_enabled.store(defaults.vsync_enabled);
        live_settings_->video_rotation.store(defaults.video_rotation);
        live_settings_->auto_recover_seconds.store(defaults.auto_recover_seconds);
        live_settings_->fullscreen.store(defaults.fullscreen);
        live_settings_->chromeless.store(defaults.chromeless);
        preferences = defaults.ui;
        live_settings_->set_ui_preferences(preferences);
        std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s", defaults.receiver_name.c_str());
        std::snprintf(recording_dir.data(), recording_dir.size(), "%s", default_recording_dir.c_str());
        recording_options = VideoRecorder::Options{};
        ui.diagnostics = defaults.ui.diagnostics;
        ui.chromeless = false;
        custom_resolution = false;
        prev_chromeless = false;
        saved_window_w = saved_window_h = 0;
        fullscreen = false;
        SDL_SetWindowFullscreen(window, 0);
        SDL_ShowCursor(SDL_ENABLE);
        SDL_RestoreWindow(window);
        SDL_SetWindowMinimumSize(window, 980, 720);
        saved_window_maximized = false;
        SDL_SetWindowSize(window, defaults.ui.window_width, defaults.ui.window_height);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        publish_preferences();
        settings_reset_notice_until = ImGui::GetTime() + 4.0;
    };

    auto rotation_degrees = [&]() {
        const int mode = live_settings_ ? live_settings_->video_rotation.load() : -1;
        return mode >= 0 && mode <= 3 ? mode * 90 : 0;
    };
    auto display_width = [&]() {
        return rotation_degrees() % 180 ? video_tex_h : video_tex_w;
    };
    auto display_height = [&]() {
        return rotation_degrees() % 180 ? video_tex_w : video_tex_h;
    };

    auto snap_to_video_aspect = [&]() {
        if (!ui.chromeless || fullscreen) return;
        if (video_tex_w <= 0 || video_tex_h <= 0) return;
        int wnow = 0, hnow = 0;
        SDL_GetWindowSize(window, &wnow, &hnow);
        if (wnow <= 0 || hnow <= 0) return;
        const float vid_a = float(display_width()) / float(display_height());
        const float win_a = float(wnow) / float(hnow);
        int snap_w = wnow, snap_h = hnow;
        if (win_a > vid_a) {
            snap_w = static_cast<int>(hnow * vid_a + 0.5f);
        } else {
            snap_h = static_cast<int>(wnow / vid_a + 0.5f);
        }
        if (std::abs(snap_w - wnow) > 1 || std::abs(snap_h - hnow) > 1) {
            SDL_SetWindowSize(window, snap_w, snap_h);
        }
    };

    // Dynamic window aspect-ratio adjustment when iOS or Android rotates between
    // Portrait and Landscape. Works in both chromeless and regular windowed mode!
    auto snap_preserving_area = [&](bool force_windowed = false) {
        if (fullscreen) return;
        if (!ui.chromeless && !force_windowed) return;
        if (video_tex_w <= 0 || video_tex_h <= 0) return;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) return;
        int wnow = 0, hnow = 0;
        SDL_GetWindowSize(window, &wnow, &hnow);
        if (wnow <= 0 || hnow <= 0) return;
        const float vid_a = float(display_width()) / float(display_height());
        if (ui.chromeless) {
            const int long_dim = std::max(wnow, hnow);
            int new_w, new_h;
            if (vid_a >= 1.0f) {
                new_w = long_dim;
                new_h = static_cast<int>(long_dim / vid_a + 0.5f);
            } else {
                new_h = long_dim;
                new_w = static_cast<int>(long_dim * vid_a + 0.5f);
            }
            if (std::abs(new_w - wnow) > 1 || std::abs(new_h - hnow) > 1) {
                SDL_SetWindowSize(window, new_w, new_h);
            }
        } else {
            // In chrome mode, smoothly adjust window width/height when orientation flips
            const int target_w = (vid_a >= 1.0f) ? std::max(wnow, 1240) : std::max(980, std::min(wnow, 1060));
            const int target_h = (vid_a >= 1.0f) ? std::max(760, std::min(hnow, 840)) : std::max(hnow, 860);
            if (std::abs(target_w - wnow) > 8 || std::abs(target_h - hnow) > 8) {
                SDL_SetWindowSize(window, target_w, target_h);
            }
        }
    };

    ap::platform::FirewallMonitor firewall_monitor;
    while (running_.load()) {
        if (show_requested_.exchange(false)) {
            SDL_ShowWindow(window);
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
                SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
#if defined(_WIN32)
            if (instance_window_info.info.win.window)
                SetForegroundWindow(instance_window_info.info.win.window);
#endif
        }

        if (preferences.dark_mode != applied_dark_mode) {
            apply_overlay_theme(preferences.dark_mode);
            configure_win11_window_chrome(window, preferences.dark_mode);
            applied_dark_mode = preferences.dark_mode;
        }

        const bool video_changed =
            (display_width() != prev_video_w || display_height() != prev_video_h) &&
            display_width() > 0 && display_height() > 0;
        if (ui.chromeless != prev_chromeless) {
            SDL_SetWindowMinimumSize(window, ui.chromeless ? 160 : 980, ui.chromeless ? 120 : 720);
            if (ui.chromeless && !fullscreen) {
                saved_window_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
                if (saved_window_maximized) SDL_RestoreWindow(window);
                SDL_GetWindowSize(window, &saved_window_w, &saved_window_h);
                snap_to_video_aspect();
            } else if (!ui.chromeless && saved_window_w > 0 && saved_window_h > 0) {
                SDL_SetWindowSize(window, saved_window_w, saved_window_h);
                if (saved_window_maximized) SDL_MaximizeWindow(window);
            }
            prev_chromeless = ui.chromeless;
            if (live_settings_)
                live_settings_->chromeless.store(ui.chromeless, std::memory_order_relaxed);
            prev_video_w = display_width();
            prev_video_h = display_height();
        } else if (video_changed) {
            const bool aspect_flipped =
                (prev_video_w > 0 && prev_video_h > 0) &&
                ((prev_video_w > prev_video_h) != (display_width() > display_height()));
            snap_preserving_area(aspect_flipped);
            prev_video_w = display_width();
            prev_video_h = display_height();
        }

        if (live_settings_) {
            const bool want_vsync =
                live_settings_->vsync_enabled.load(std::memory_order_relaxed) &&
                !vsync_relief;
            if (want_vsync != vsync_active) {
                if (SDL_RenderSetVSync(renderer, want_vsync ? 1 : 0) == 0) {
                    LOG_INFO << "VideoRenderer VSYNC "
                             << (want_vsync ? "enabled" : "disabled");
                    vsync_active = want_vsync;
                }
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                LOG_INFO << "VideoRenderer received SDL_QUIT";
                closed_  = true;
                running_ = false;
                break;
            }
            if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.x != prev_mouse_x || e.motion.y != prev_mouse_y) {
                    prev_mouse_x = e.motion.x;
                    prev_mouse_y = e.motion.y;
                    last_mouse_move_time = std::chrono::steady_clock::now();
                }
            }
            const ImGuiIO& io_poll = ImGui::GetIO();
            const bool receiver_shortcut = e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_f || e.key.keysym.sym == SDLK_F11 ||
                 e.key.keysym.sym == SDLK_h || e.key.keysym.sym == SDLK_r ||
                 e.key.keysym.sym == SDLK_s);
            const bool allow_keyboard = receiver_shortcut
                ? !io_poll.WantTextInput &&
                  !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
                : !io_poll.WantCaptureKeyboard;
            if (e.type == SDL_KEYDOWN && !e.key.repeat && allow_keyboard) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        if (fullscreen)            toggle_fullscreen();
                        else if (ui.chromeless)    ui.chromeless = false;
                        else if (ui.settings_open) ui.settings_open = false;
                        else { closed_ = true; running_ = false; }
                        break;
                    case SDLK_f:
                    case SDLK_F11:
                        toggle_fullscreen();
                        break;
                    case SDLK_h:
                        ui.chromeless = !ui.chromeless;
                        break;
                    case SDLK_r:
                        toggle_recording();
                        break;
                    case SDLK_s:
                        screenshot_requested = true;
                        break;
                    default: break;
                }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN &&
                e.button.button == SDL_BUTTON_LEFT &&
                e.button.clicks == 2 &&
                !io_poll.WantCaptureMouse) {
                toggle_fullscreen();
            }
        }
        if (!running_.load()) break;

        // --- Consume pending video frame -----------------------------
        bool have_avf   = false;
        bool have_frame = false;
        bool have_nv12  = false;
        std::vector<unsigned char> y, u, v, uv;
        DecodedFrame avf_local;
        int w = 0, h = 0;
        int64_t frame_origin_ns_local = 0;
        DeviceInfo pacing_device;
        const bool audio_only = active_device_snapshot(pacing_device) && pacing_device.kind == "Audio";
        const auto refresh_wait = std::chrono::milliseconds(audio_only ? 33 : 16);
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, refresh_wait,
                         [this] {
                             return has_avframe_ || has_frame_ ||
                                    cover_dirty_ || !running_.load();
                         });
            if (has_avframe_) {
                avf_local       = std::move(slot_avframe_);
                w               = frame_w_;
                h               = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_avframe_    = false;
                have_avf        = true;
            } else if (has_frame_) {
                have_nv12 = frame_is_nv12_;
                if (have_nv12) {
                    y  = y_buf_;
                    uv = uv_buf_;
                } else {
                    y = y_buf_; u = u_buf_; v = v_buf_;
                }
                w = frame_w_; h = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_frame_ = false;
                have_frame = true;
            }
        }

        Uint32 desired_fmt = SDL_PIXELFORMAT_IYUV;
        AVFrame* af = avf_local.frame;
        if (have_avf && af) {
            if (af->format == AV_PIX_FMT_D3D11) {
                desired_fmt = SDL_PIXELFORMAT_ARGB8888;
            } else if (af->format == AV_PIX_FMT_NV12) {
                desired_fmt = SDL_PIXELFORMAT_NV12;
            } else {
                desired_fmt = SDL_PIXELFORMAT_IYUV;
            }
        } else if (have_frame) {
            desired_fmt = have_nv12 ? SDL_PIXELFORMAT_NV12
                                    : SDL_PIXELFORMAT_IYUV;
        }

        if (have_avf || have_frame) {
            if (!video_tex || video_tex_w != w || video_tex_h != h ||
                video_tex_fmt != desired_fmt) {
                gpu_video.reset_target();
                if (video_tex) SDL_DestroyTexture(video_tex);
                video_tex = SDL_CreateTexture(renderer,
                    desired_fmt, desired_fmt == SDL_PIXELFORMAT_ARGB8888
                        ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING, w, h);
                video_tex_w   = w;
                video_tex_h   = h;
                video_tex_fmt = desired_fmt;
                LOG_INFO << "VideoRenderer texture ("
                         << (desired_fmt == SDL_PIXELFORMAT_NV12 ? "NV12"
                             : desired_fmt == SDL_PIXELFORMAT_ARGB8888 ? "GPU BGRA"
                                                                       : "IYUV")
                         << ") created " << w << 'x' << h;
            }
            if (video_tex) {
                if (have_avf && af) {
                    if (af->format == AV_PIX_FMT_D3D11) {
                        if (!gpu_video.convert(renderer, video_tex, af)) {
                            const int streak = gpu_convert_failures_.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                            if (streak == 1 || streak % 120 == 0)
                                LOG_ERROR << "D3D11 surface conversion failed (in a row: "
                                          << streak << ')';
                            gpu_video.reset_target();
                            SDL_DestroyTexture(video_tex);
                            video_tex = nullptr;
                        } else {
                            gpu_convert_failures_.store(0, std::memory_order_relaxed);
                            frames_presented_.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (af->format == AV_PIX_FMT_NV12) {
                        SDL_UpdateNVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1]);
                        frames_presented_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        SDL_UpdateYUVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1],
                            af->data[2], af->linesize[2]);
                        frames_presented_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (have_nv12) {
                    SDL_UpdateNVTexture(video_tex, nullptr,
                        y.data(), w, uv.data(), w);
                } else {
                    SDL_UpdateYUVTexture(video_tex, nullptr,
                        y.data(), w, u.data(), w / 2, v.data(), w / 2);
                }
            }
            last_video_frame_time = std::chrono::steady_clock::now();
        }

        if (clear_cover_requested_.exchange(false, std::memory_order_relaxed)) {
            if (cover_tex) {
                SDL_DestroyTexture(cover_tex);
                cover_tex   = nullptr;
                cover_tex_w = cover_tex_h = 0;
            }
            std::lock_guard<std::mutex> lock(cover_mtx_);
            cover_jpeg_.clear();
            cover_dirty_ = false;
        }

        std::vector<unsigned char> pending_cover;
        {
            std::lock_guard<std::mutex> lock(cover_mtx_);
            if (cover_dirty_) {
                pending_cover = std::move(cover_jpeg_);
                cover_jpeg_.clear();
                cover_dirty_ = false;
            }
        }

        std::string t_title, t_artist, t_album, t_idle_name, t_idle_ip;
        bool meta_changed = false;
        bool idle_changed = false;
        {
            std::lock_guard<std::mutex> lock(meta_mtx_);
            if (meta_dirty_) {
                t_title      = meta_title_;
                t_artist     = meta_artist_;
                t_album      = meta_album_;
                meta_dirty_  = false;
                meta_changed = true;
            }
            if (idle_dirty_) {
                t_idle_name  = idle_name_;
                t_idle_ip    = idle_ip_;
                idle_dirty_  = false;
                idle_changed = true;
            }
        }
        if (idle_changed) {
            make_text(fonts_big,   t_idle_name,
                      SDL_Color{255, 255, 255, 255},
                      idle_name_tex, idle_name_w, idle_name_h);
            make_text(fonts_small, t_idle_ip,
                      SDL_Color{180, 180, 180, 255},
                      idle_ip_tex,   idle_ip_w,   idle_ip_h);
            make_text(fonts_small, ap::i18n::tr(ap::i18n::Str::StatusWaitingForAirPlay),
                      SDL_Color{150, 150, 150, 255},
                      idle_msg_tex,  idle_msg_w,  idle_msg_h);
        }
        if (meta_changed) {
            make_text(fonts_big,   t_title,  SDL_Color{255, 255, 255, 255},
                      title_tex,  title_w,  title_h);
            make_text(fonts_small, t_artist, SDL_Color{220, 220, 220, 255},
                      artist_tex, artist_w, artist_h);
            make_text(fonts_small, t_album,  SDL_Color{180, 180, 180, 255},
                      album_tex,  album_w,  album_h);
            desktop_controls.metadata(t_title, t_artist, t_album);
        }

        if (!pending_cover.empty()) {
            desktop_controls.artwork(pending_cover.data(), pending_cover.size());
            int cw = 0, ch = 0;
            std::vector<uint8_t> rgb;
            if (decode_cover_art(pending_cover.data(), pending_cover.size(),
                                 cw, ch, rgb)) {
                if (!cover_tex || cover_tex_w != cw || cover_tex_h != ch) {
                    if (cover_tex) SDL_DestroyTexture(cover_tex);
                    cover_tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, cw, ch);
                    cover_tex_w = cw; cover_tex_h = ch;
                }
                if (cover_tex) {
                    SDL_UpdateTexture(cover_tex, nullptr, rgb.data(), cw * 3);
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const bool video_fresh =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_video_frame_time).count() < 500;
        const bool playing_now = playing_.load(std::memory_order_relaxed);
        DeviceInfo active_dev;
        const bool has_active_dev = active_device_snapshot(active_dev);
        if (has_active_dev != keep_awake_on) {
            keep_awake_on = has_active_dev;
            ap::platform::set_keep_awake(keep_awake_on);
        }
        const bool show_video =
            video_tex && active_dev.kind != "Audio" && (video_fresh || has_active_dev || !playing_now);

        desktop_controls.publish(has_active_dev && active_dev.kind == "Audio",
                                 playing_now,
                                 audio_control_available_.load(),
                                 progress_elapsed_ms_.load(),
                                 progress_total_ms_.load());

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
        const int full_w = win_w;
        const int full_h = win_h;

        const bool no_chrome = fullscreen || ui.chromeless;
        const auto firewall = firewall_monitor.status();
        const bool firewall_notice =
            firewall.state != ap::platform::FirewallState::Checking &&
            firewall.state != ap::platform::FirewallState::Allowed;
        const int firewall_offset = firewall_notice ? (firewall.message.empty() ? 116 : 140) : 0;
        const int chrome_top = no_chrome ? 0 : kToolbarHeight;
        const int diagnostics_columns = std::clamp((full_w - 20) / 300, 1, 4);
        const int diagnostics_rows = (9 + diagnostics_columns - 1) / diagnostics_columns;
        const int chrome_bottom = no_chrome || !ui.diagnostics
            ? 0 : diagnostics_rows * kStatusLineHeight + 8;
        const int chrome_left = no_chrome ? 0 : kSidebarWidth;
        const int stage_x = chrome_left;
        const int stage_y = chrome_top;
        const int stage_w = std::max(0, full_w - chrome_left);
        const int stage_h = std::max(0, full_h - chrome_top - chrome_bottom);

        const auto pal = receiver_ui::get_palette(preferences.dark_mode);
        if (no_chrome) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        } else if (preferences.dark_mode) {
            SDL_SetRenderDrawColor(renderer, 17, 20, 25, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 243, 245, 248, 255);
        }
        SDL_RenderClear(renderer);

        const bool framed_video = !no_chrome && show_video;
        const SDL_Rect stage_rect{
            stage_x + (framed_video ? 36 : 0),
            stage_y + (framed_video ? 88 + firewall_offset : 0),
            std::max(1, stage_w - (framed_video ? 72 : 0)),
            std::max(1, stage_h - (framed_video ? 148 + firewall_offset : 0))
        };
        win_w = stage_rect.w;
        win_h = stage_rect.h;
        SDL_RenderSetViewport(renderer, &stage_rect);

        auto draw_pause_badge = [&](const SDL_Rect& dst) {
            const int badge = std::min(dst.w, dst.h) / 6;
            const int cx    = dst.x + dst.w / 2;
            const int cy    = dst.y + dst.h / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect bg{cx - badge, cy - badge, badge * 2, badge * 2};
            SDL_RenderFillRect(renderer, &bg);
            const int bw = badge / 3;
            const int bh = badge;
            const int gap = badge / 3;
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 235);
            SDL_Rect b1{cx - gap - bw, cy - bh / 2, bw, bh};
            SDL_Rect b2{cx + gap,      cy - bh / 2, bw, bh};
            SDL_RenderFillRect(renderer, &b1);
            SDL_RenderFillRect(renderer, &b2);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        };

        const bool show_pause_badge =
            !playing_now &&
            (!has_active_dev || (active_dev.kind != "Mirror" &&
                                 active_dev.kind != "USB" &&
                                 active_dev.kind != "Android"));

        SDL_Rect rendered_video_dst{0, 0, win_w, win_h};
        if (show_video) {
            SDL_SetRenderDrawColor(renderer, 9, 11, 14, 255);
            SDL_RenderFillRect(renderer, nullptr);
            const int angle = rotation_degrees();
            SDL_Rect dst = fit_inside(display_width(), display_height(), win_w, win_h);
            rendered_video_dst = dst;
            SDL_Rect copy_dst = dst;
            if (angle % 180) {
                copy_dst.w = dst.h;
                copy_dst.h = dst.w;
                copy_dst.x = dst.x + (dst.w - copy_dst.w) / 2;
                copy_dst.y = dst.y + (dst.h - copy_dst.h) / 2;
            }
            SDL_RenderCopyEx(renderer, video_tex, nullptr, &copy_dst,
                             angle, nullptr, SDL_FLIP_NONE);
            if (show_pause_badge) {
                draw_pause_badge(dst);
            }
        } else if (cover_tex && no_chrome) {
            const int strip_h = std::min(win_h, std::max(win_h / 4, 230));
            const int top_h   = win_h - strip_h;
            SDL_Rect dst = fit_inside(cover_tex_w, cover_tex_h, win_w, top_h);
            SDL_RenderCopy(renderer, cover_tex, nullptr, &dst);
            if (show_pause_badge) draw_pause_badge(dst);
            const int pad_x = 20;
            int y_pos = top_h + 10;
            auto draw_text = [&](SDL_Texture* tex, int tw, int th) {
                if (!tex) return;
                int max_w = win_w - 2 * pad_x;
                int w_clamped = std::min(tw, max_w);
                SDL_Rect r{pad_x, y_pos, w_clamped, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
                y_pos += th + 4;
            };
            draw_text(title_tex,  title_w,  title_h);
            draw_text(artist_tex, artist_w, artist_h);
            draw_text(album_tex,  album_w,  album_h);
        } else if (no_chrome) {
            auto draw_centered = [&](SDL_Texture* tex, int tw, int th, int y_pos) {
                if (!tex) return;
                SDL_Rect r{(win_w - tw) / 2, y_pos, tw, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
            };
            const int total_h = idle_name_h + idle_ip_h + idle_msg_h + 24;
            int y_cursor = (win_h - total_h) / 2;
            draw_centered(idle_name_tex, idle_name_w, idle_name_h, y_cursor);
            y_cursor += idle_name_h + 12;
            draw_centered(idle_ip_tex,   idle_ip_w,   idle_ip_h,   y_cursor);
            y_cursor += idle_ip_h + 12;
            draw_centered(idle_msg_tex,  idle_msg_w,  idle_msg_h,  y_cursor);
        }

        // Handle instant screenshot capture request before ImGui overlays are drawn
        if (screenshot_requested) {
            screenshot_requested = false;
            SDL_Rect snap_area{
                stage_rect.x + rendered_video_dst.x,
                stage_rect.y + rendered_video_dst.y,
                std::max(1, rendered_video_dst.w),
                std::max(1, rendered_video_dst.h)
            };
            if (!show_video) {
                snap_area = SDL_Rect{0, 0, full_w, full_h};
            }
            SDL_RenderSetViewport(renderer, nullptr);
            last_screenshot_path = capture_renderer_screenshot(
                renderer, snap_area, recording_dir.data());
            if (!last_screenshot_path.empty()) {
                screenshot_toast_until = ImGui::GetTime() + 3.5;
            }
        }

        SDL_RenderSetViewport(renderer, nullptr);
        win_w = full_w;
        win_h = full_h;

        // --- Dear ImGui overlay pass ---------------------------------
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiWindowFlags kPanelFlags =
            ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoMove        |
            ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoTitleBar    |
            ImGuiWindowFlags_NoNavFocus  | ImGuiWindowFlags_NoSavedSettings;

        const float status_bar_h = static_cast<float>(chrome_bottom);
        const float toolbar_h    = static_cast<float>(kToolbarHeight);
        const float side_bar_w   = static_cast<float>(kSidebarWidth);
        const float opts_bar_w   = static_cast<float>(std::min(kOptionsWidth, full_w / 2));
        const float fw           = static_cast<float>(full_w);
        const float fh           = static_cast<float>(full_h);

        namespace ui_draw = receiver_ui;
        using ap::i18n::Str;
        using ap::i18n::tr;

        const char* rotation_labels[5] = {
            tr(Str::SettingsRotationAuto),
            tr(Str::SettingsRotation0),
            tr(Str::SettingsRotation90),
            tr(Str::SettingsRotation180),
            tr(Str::SettingsRotation270)
        };

        auto draw_audio_controls = [&]() {
            const bool ready = audio_control_available_.load();
            const bool pending = audio_command_pending_.load();
            const uint32_t total = progress_total_ms_.load();
            uint64_t elapsed = progress_elapsed_ms_.load();
            const int64_t pushed = progress_pushed_at_ns_.load();
            if (playing_now && pushed > 0) {
                const auto clock_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                if (clock_ns > pushed) elapsed += (clock_ns - pushed) / 1000000;
            }
            elapsed = total > 0 ? std::min<uint64_t>(elapsed, total) : 0;
            const float progress = total > 0 ? static_cast<float>(elapsed) / total : 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(p, ImVec2(p.x + width, p.y + 4), pal.border_line, 2);
            if (progress > 0) draw->AddRectFilled(p, ImVec2(p.x + width * progress, p.y + 4), pal.accent_blue, 2);
            ImGui::Dummy(ImVec2(width, 6));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(Str::MusicProgressTooltip));
            ImGui::PushFont(small_font);
            ImGui::TextDisabled("%u:%02u", static_cast<unsigned>(elapsed / 60000), static_cast<unsigned>((elapsed / 1000) % 60));
            char duration[32]; std::snprintf(duration, sizeof(duration), "%u:%02u", total / 60000, (total / 1000) % 60);
            ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(duration).x);
            ImGui::TextDisabled("%s", duration);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 6));
            auto command = [&](ap::airplay::MediaCommand action) {
                audio_command_failed_.store(false); audio_command_pending_.store(true); audio_control_handler_(action, 0);
            };
            const ImVec2 row = ImGui::GetCursorPos();
            ImGui::BeginDisabled(!ready || pending);
            ImGui::SetCursorPos(ImVec2(row.x, row.y + 10.5f));
            if (ui_draw::icon_button("##previous", ui_draw::Icon::Previous, tr(Str::MusicPrevTooltip), 34, false, preferences.dark_mode))
                command(ap::airplay::MediaCommand::Previous);
            ImGui::SetCursorPos(ImVec2(row.x + 52, row.y));
            if (ui_draw::icon_button("##play", playing_now ? ui_draw::Icon::Pause : ui_draw::Icon::Play,
                                     playing_now ? tr(Str::MusicPauseTooltip) : tr(Str::MusicPlayTooltip),
                                     54, true, preferences.dark_mode))
                command(ap::airplay::MediaCommand::PlayPause);
            ImGui::SetCursorPos(ImVec2(row.x + 124, row.y + 10.5f));
            if (ui_draw::icon_button("##next", ui_draw::Icon::Next, tr(Str::MusicNextTooltip), 34, false, preferences.dark_mode))
                command(ap::airplay::MediaCommand::Next);
            ImGui::EndDisabled();
            ImGui::SetCursorPos(ImVec2(row.x, row.y + 65));
            ImGui::PushFont(small_font);
            if (pending) ImGui::TextDisabled("%s", tr(Str::MusicSendingCmd));
            else if (!ready) ImGui::TextDisabled("%s", tr(Str::MusicRemoteUnavailable));
            else if (audio_command_failed_.load()) ImGui::TextDisabled("%s", tr(Str::MusicCmdFailed));
            ImGui::PopFont();
        };

        // Snapshot live USB supervisor state
        int usb_state_local = 0;
        std::string usb_dev_name, usb_ident;
        {
            std::lock_guard<std::mutex> lock(usb_mtx_);
            usb_state_local = usb_state_;
            usb_dev_name    = usb_device_name_;
            usb_ident       = usb_identifier_;
        }

        if (!no_chrome) {
            if (has_active_dev && !ui.was_connected) ui.page = active_dev.kind == "Audio" ? 1 : 2;
            if (!has_active_dev && ui.was_connected) ui.page = 0;
            ui.was_connected = has_active_dev;
            const char* receiver = receiver_name_buf.data();

            // ---- Top Title Bar (with Quick EN/VI Switcher & Dark/Light Mode Toggle) ----
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(fw, toolbar_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_sidebar));
            ImGui::Begin("##titlebar", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar);
            auto* title_draw = ImGui::GetWindowDrawList();
            ui_draw::icon(title_draw, ui_draw::Icon::Cast, ImVec2(18, 13), 20, pal.accent_blue);
            title_draw->AddText(bold_font, 16.5f, ImVec2(48, 13), pal.text_ink, ap::kAppName);
            title_draw->AddLine(ImVec2(0, toolbar_h - 1), ImVec2(fw, toolbar_h - 1), pal.border_line);

            // Instant EN / VI Language Switcher + Dark/Light Theme Button on the Title Bar
            ImGui::SetCursorPos(ImVec2(fw - 252.0f, 7.0f));
            const bool is_vi = (ap::i18n::current_language() == ap::i18n::Language::Vietnamese);
            if (ui_draw::pill_button("##title_lang", is_vi ? "VI" : "EN",
                                     ui_draw::Icon::Globe, is_vi,
                                     tr(Str::BtnLangSwitchTooltip),
                                     small_font, preferences.dark_mode, 31.0f)) {
                ap::i18n::LocalizationManager::instance().toggle_language();
                preferences.language = static_cast<int>(ap::i18n::current_language());
                idle_dirty_ = true;
            }
            ImGui::SameLine(0, 6.0f);
            if (ui_draw::icon_button("##title_theme", ui_draw::Icon::Theme,
                                     preferences.dark_mode ? tr(Str::BtnThemeLight) : tr(Str::BtnThemeDark),
                                     31.0f, false, preferences.dark_mode)) {
                preferences.dark_mode = !preferences.dark_mode;
            }

            ImGui::SetCursorPos(ImVec2(fw - 144.0f, 0.0f));
            auto window_control = [&](const char* id, ui_draw::Icon kind) {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const bool click = ui_draw::surface(id, ImVec2(48, toolbar_h - 1));
                if (ImGui::IsItemHovered()) {
                    title_draw->AddRectFilled(
                        pos, ImVec2(pos.x + 48, pos.y + toolbar_h - 1),
                        kind == ui_draw::Icon::Close ? IM_COL32(220, 53, 69, 210) : pal.bg_card_hover);
                }
                const ImU32 ic_col = (ImGui::IsItemHovered() && kind == ui_draw::Icon::Close)
                    ? IM_COL32(255, 255, 255, 255) : pal.text_muted;
                ui_draw::icon(title_draw, kind, ImVec2(pos.x + 14, pos.y + 13), 20, ic_col);
                return click;
            };
            if (window_control("##minimize", ui_draw::Icon::Minimize)) SDL_MinimizeWindow(window);
            ImGui::SameLine(0, 0);
            if (window_control("##maximize", ui_draw::Icon::Maximize)) {
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
                else SDL_MaximizeWindow(window);
            }
            ImGui::SameLine(0, 0);
            if (window_control("##close", ui_draw::Icon::Close)) {
                SDL_Event event{}; event.type = SDL_QUIT; SDL_PushEvent(&event);
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            // ---- Left Navigation Rail ----
            ImGui::SetNextWindowPos(ImVec2(0, toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(side_bar_w, fh - toolbar_h - status_bar_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 24));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_sidebar));
            ImGui::Begin("##navigation", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar);
            auto* rail = ImGui::GetWindowDrawList();
            rail->AddLine(ImVec2(side_bar_w - 1, toolbar_h), ImVec2(side_bar_w - 1, fh - status_bar_h), pal.border_line);
            ImGui::PushFont(small_font);
            ImGui::SetCursorPosX(24);
            ImGui::TextDisabled("%s", tr(Str::YourSpace));
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 4));
            if (ui_draw::nav("##home", tr(Str::NavHome), ui_draw::Icon::Home,
                             ui.page == 0, 172, ui.page == 0 ? bold_font : io.FontDefault, preferences.dark_mode)) {
                ui.page = 0;
            }
            if (ui_draw::nav("##music", tr(Str::NavMusic), ui_draw::Icon::Music,
                             ui.page == 1, 172, ui.page == 1 ? bold_font : io.FontDefault, preferences.dark_mode)) {
                ui.page = 1;
            }
            if (ui_draw::nav("##mirror", tr(Str::NavMirror), ui_draw::Icon::Screen,
                             ui.page == 2, 172, ui.page == 2 ? bold_font : io.FontDefault, preferences.dark_mode)) {
                ui.page = 2;
            }
            ImGui::Dummy(ImVec2(0, 6));
            if (ui_draw::nav("##usb_mode", tr(Str::NavUsb), ui_draw::Icon::Usb,
                             preferences.connection_mode == 1, 172, io.FontDefault, preferences.dark_mode)) {
                preferences.connection_mode = (preferences.connection_mode == 1) ? 0 : 1;
            }
            if (ui_draw::nav("##android_mode", tr(Str::NavAndroid), ui_draw::Icon::Android,
                             preferences.platform_mode == 1, 172, io.FontDefault, preferences.dark_mode)) {
                preferences.platform_mode = (preferences.platform_mode == 1) ? 0 : 1;
            }

            ImGui::SetCursorPosY(fh - toolbar_h - status_bar_h - 148);
            if (ui_draw::nav("##settings", tr(Str::NavSettings), ui_draw::Icon::Settings,
                             ui.settings_open, 172, io.FontDefault, preferences.dark_mode)) {
                ui.settings_open = !ui.settings_open;
            }
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushFont(small_font);
            ImVec2 p = ImGui::GetCursorScreenPos();
            rail->AddCircleFilled(ImVec2(p.x + 11, p.y + 8), 3.8f, pal.status_green);
            ImGui::SetCursorPosX(36);
            ImGui::TextDisabled("%s", has_active_dev ? tr(Str::StatusDeviceConnected) : tr(Str::StatusReceiverAvailable));
            ImGui::SetCursorPosX(25);
            ImGui::PushTextWrapPos(side_bar_w - 15);
            ImGui::TextUnformatted(receiver);
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            // ---- Main Stage Content ----
            const bool music_view = ui.page == 1 && has_active_dev && active_dev.kind == "Audio";
            const bool video_view = (ui.page == 2 || has_active_dev) && show_video;
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(stage_x), toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(stage_w), static_cast<float>(stage_h)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_stage));
            ImGui::Begin("##content", nullptr, kPanelFlags | (video_view ? ImGuiWindowFlags_NoBackground : 0));
            auto* draw = ImGui::GetWindowDrawList();
            ImVec2 origin = ImGui::GetWindowPos();
            origin.y -= ImGui::GetScrollY();
            const float width = stage_w - 72.0f;

            auto text_at = [&](ImFont* font, float x, float y_pos, ImU32 color, const char* text) {
                draw->AddText(font, font->FontSize, ImVec2(origin.x + x, origin.y + y_pos), color, text);
            };
            auto center_text = [&](ImFont* font, float y_pos, ImU32 color, const char* text) {
                const float tw = font->CalcTextSizeA(font->FontSize, 10000, 0, text).x;
                text_at(font, (stage_w - tw) * 0.5f, y_pos, color, text);
            };

            if (music_view || video_view) {
                draw->AddRectFilled(ImVec2(origin.x + 36, origin.y + 24),
                                    ImVec2(origin.x + 76, origin.y + 64), pal.bg_card, 10.0f);
                draw->AddRect(ImVec2(origin.x + 36, origin.y + 24),
                              ImVec2(origin.x + 76, origin.y + 64), pal.border_line, 10.0f);
                const ui_draw::Icon dev_icon =
                    (active_dev.kind == "Android") ? ui_draw::Icon::Android :
                    (active_dev.kind == "USB")     ? ui_draw::Icon::Usb     :
                                                     ui_draw::Icon::Phone;
                ui_draw::icon(draw, dev_icon, ImVec2(origin.x + 46, origin.y + 34), 20, pal.accent_blue);
                text_at(bold_font, 88, 25, pal.text_ink,
                        active_dev.name.empty() ? tr(Str::StatusDeviceConnected) : active_dev.name.c_str());
                draw->AddCircleFilled(ImVec2(origin.x + 92, origin.y + 54), 3.2f, pal.status_green);
                const char* sub_status =
                    music_view                     ? tr(Str::StatusConnectedAudio)   :
                    (active_dev.kind == "USB")     ? tr(Str::StatusConnectedUsb)     :
                    (active_dev.kind == "Android") ? tr(Str::StatusConnectedAndroid) :
                                                     tr(Str::StatusConnectedMirror);
                text_at(small_font, 102, 45, pal.text_muted, sub_status);

                ImGui::SetCursorPos(ImVec2(stage_w - 148.0f, 27.0f));
                if (ui_draw::text_button(tr(Str::BtnDisconnect), ImVec2(112, 34), preferences.dark_mode) &&
                    disconnect_handler_) {
                    disconnect_handler_();
                }

                // Device frame bezel around the mirrored video when enabled in Preferences
                if (video_view && preferences.device_frame) {
                    const ImVec2 f_tl(static_cast<float>(stage_rect.x + rendered_video_dst.x - 4),
                                      static_cast<float>(stage_rect.y + rendered_video_dst.y - 4));
                    const ImVec2 f_br(static_cast<float>(stage_rect.x + rendered_video_dst.x + rendered_video_dst.w + 4),
                                      static_cast<float>(stage_rect.y + rendered_video_dst.y + rendered_video_dst.h + 4));
                    draw->AddRect(f_tl, f_br, pal.border_glass, 14.0f, 0, 2.5f);
                }
            } else {
                const char* page_heading =
                    ui.page == 0 ? tr(Str::PageTitleHome) :
                    ui.page == 1 ? tr(Str::PageTitleMusic) :
                                   tr(Str::PageTitleMirror);
                text_at(page_font, 36, 30, pal.text_ink, page_heading);
                const float badge_w = 158.0f;
                const float badge_x = stage_w - 36.0f - badge_w;
                const bool firewall_attention = firewall_notice && !has_active_dev;
                draw->AddRectFilled(ImVec2(origin.x + badge_x, origin.y + 30),
                                    ImVec2(origin.x + stage_w - 36, origin.y + 60),
                                    firewall_attention ? pal.status_amber_bg : pal.status_green_bg, 15.0f);
                draw->AddRect(ImVec2(origin.x + badge_x, origin.y + 30),
                              ImVec2(origin.x + stage_w - 36, origin.y + 60),
                              firewall_attention ? pal.status_amber : pal.status_green, 15.0f);
                draw->AddCircleFilled(ImVec2(origin.x + badge_x + 14, origin.y + 45), 3.8f,
                                      firewall_attention ? pal.status_amber : pal.status_green);
                const char* pill_lbl =
                    has_active_dev     ? tr(Str::StatusConnected) :
                    firewall_attention ? tr(Str::StatusCheckFirewall) :
                                         tr(Str::StatusReadyToReceive);
                text_at(small_font, badge_x + 26, 37,
                        firewall_attention ? pal.status_amber : pal.status_green, pill_lbl);
            }

            if (firewall_notice) {
                const bool can_allow = firewall.state == ap::platform::FirewallState::NeedsRule;
                const float banner_y = 76.0f;
                const float banner_h = firewall.message.empty() ? 98.0f : 122.0f;
                const ImVec2 a(origin.x + 36, origin.y + banner_y);
                const ImVec2 b(origin.x + stage_w - 36, origin.y + banner_y + banner_h);
                draw->AddRectFilled(a, b, pal.status_amber_bg, 12.0f);
                draw->AddRect(a, b, pal.status_amber, 12.0f);
                const char* banner_title =
                    can_allow ? tr(Str::FirewallBannerAllowTitle) :
                    firewall.state == ap::platform::FirewallState::Managed
                        ? tr(Str::FirewallBannerBlockedTitle)
                        : tr(Str::FirewallBannerUnavailableTitle);
                text_at(bold_font, 52, banner_y + 14, pal.status_amber, banner_title);
                const char* detail = can_allow ? tr(Str::FirewallBannerAllowDetail) : firewall.message.c_str();
                draw->AddText(small_font, 15.0f, ImVec2(a.x + 16, a.y + 42),
                              pal.text_ink, detail, nullptr, width - 210);
                ImGui::SetCursorPos(ImVec2(stage_w - 220.0f, banner_y + 34));
                ImGui::BeginDisabled(firewall.busy);
                if (can_allow && ui_draw::text_button(
                        firewall.busy ? tr(Str::FirewallBtnWaiting) : tr(Str::FirewallBtnAllow),
                        ImVec2(170, 34), preferences.dark_mode)) {
                    firewall_monitor.allow_local_network();
                }
                ImGui::EndDisabled();
            }

            if (music_view) {
                const bool wide = stage_w >= 780;
                const float cover_size = wide ? std::min(320.0f, width * 0.42f) : 220.0f;
                const float cover_x = wide ? 36.0f : (stage_w - cover_size) * 0.5f;
                const float cover_y = (wide ? std::max(115.0f, (stage_h - cover_size) * 0.42f) : 95.0f) + firewall_offset;
                const ImVec2 tl(origin.x + cover_x, origin.y + cover_y);
                const ImVec2 br(tl.x + cover_size, tl.y + cover_size);
                draw->AddRectFilled(ImVec2(tl.x, tl.y + 8), ImVec2(br.x, br.y + 8), IM_COL32(0, 0, 0, 45), 14.0f);
                if (cover_tex) {
                    draw->AddImageRounded((ImTextureID)(intptr_t)cover_tex, tl, br,
                                          ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 14.0f);
                } else {
                    draw->AddRectFilled(tl, br, pal.bg_card, 14.0f);
                    draw->AddRect(tl, br, pal.border_line, 14.0f);
                    ui_draw::icon(draw, ui_draw::Icon::Music,
                                  ImVec2(tl.x + cover_size * 0.5f - 25, tl.y + cover_size * 0.5f - 25),
                                  50, pal.text_muted);
                }
                const float tx = wide ? cover_x + cover_size + 40.0f : 36.0f;
                float ty = wide ? cover_y + 18.0f : cover_y + cover_size + 20.0f;
                text_at(small_font, tx, ty, pal.accent_blue,
                        playing_now ? tr(Str::MusicNowPlaying) : tr(Str::MusicPaused));
                ty += 28.0f;
                auto track_line = [&](SDL_Texture* texture, int tw, int th, float scale, ImU32 tint) {
                    if (!texture || tw <= 0 || th <= 0) return;
                    const float fit = std::min(scale, (stage_w - tx - 36.0f) / tw);
                    draw->AddImage((ImTextureID)(intptr_t)texture,
                                   ImVec2(origin.x + tx, origin.y + ty),
                                   ImVec2(origin.x + tx + tw * fit, origin.y + ty + th * fit),
                                   ImVec2(0, 0), ImVec2(1, 1), tint);
                    ty += th * fit + 8.0f;
                };
                track_line(title_tex,  title_w,  title_h,  1.0f,  pal.text_ink);
                track_line(artist_tex, artist_w, artist_h, 0.82f, pal.text_muted);
                track_line(album_tex,  album_w,  album_h,  0.68f, pal.text_subtle);
                ImGui::SetCursorPos(ImVec2(tx, ty + 20.0f));
                ImGui::BeginChild("##music_transport", ImVec2(stage_w - tx - 36.0f, 160.0f), false);
                if (audio_control_handler_) draw_audio_controls();
                ImGui::EndChild();
            } else if (!video_view) {
                // ---- Minimalist Apple-Inspired Home / Onboarding Stage ----
                const float cx = stage_w * 0.5f;
                const float hero_y = (stage_h < 740 ? 78.0f : 96.0f) + firewall_offset;

                // USB Plug-and-Play live guidance banner when USB mode is selected or a USB device is plugged in
                const bool show_usb_banner = (preferences.connection_mode == 1) || (usb_state_local != 0);
                float usb_offset = 0.0f;
                if (show_usb_banner) {
                    const float ub_y = hero_y;
                    const float ub_h = 68.0f;
                    const ImVec2 ua(origin.x + 36, origin.y + ub_y);
                    const ImVec2 ub(origin.x + stage_w - 36, origin.y + ub_y + ub_h);
                    const bool usb_ready = (usb_state_local == static_cast<int>(ap::usb::UsbState::IosTrustedReady) ||
                                            usb_state_local == static_cast<int>(ap::usb::UsbState::AndroidReady));
                    const bool usb_wait  = (usb_state_local == static_cast<int>(ap::usb::UsbState::IosWaitTrust) ||
                                            usb_state_local == static_cast<int>(ap::usb::UsbState::AndroidWaitDebugging));
                    const ImU32 banner_bg = usb_ready ? pal.status_green_bg : (usb_wait ? pal.status_amber_bg : pal.bg_card);
                    const ImU32 banner_bd = usb_ready ? pal.status_green    : (usb_wait ? pal.status_amber    : pal.border_line);
                    draw->AddRectFilled(ua, ub, banner_bg, 12.0f);
                    draw->AddRect(ua, ub, banner_bd, 12.0f);
                    ui_draw::icon(draw, ui_draw::Icon::Usb, ImVec2(ua.x + 16, ua.y + 22), 24,
                                  usb_ready ? pal.status_green : (usb_wait ? pal.status_amber : pal.accent_blue));

                    const char* usb_title = tr(Str::UsbBannerScanningTitle);
                    const char* usb_desc  = tr(Str::UsbBannerScanningDesc);
                    if (usb_state_local == static_cast<int>(ap::usb::UsbState::IosWaitTrust)) {
                        usb_title = tr(Str::UsbBannerIosUntrustedTitle);
                        usb_desc  = tr(Str::UsbBannerIosUntrustedDesc);
                    } else if (usb_state_local == static_cast<int>(ap::usb::UsbState::IosTrustedReady)) {
                        usb_title = tr(Str::UsbBannerIosTrustedTitle);
                        usb_desc  = tr(Str::UsbBannerIosTrustedDesc);
                    } else if (usb_state_local == static_cast<int>(ap::usb::UsbState::AndroidWaitDebugging)) {
                        usb_title = tr(Str::UsbBannerAndroidUnauthTitle);
                        usb_desc  = tr(Str::UsbBannerAndroidUnauthDesc);
                    } else if (usb_state_local == static_cast<int>(ap::usb::UsbState::AndroidReady)) {
                        usb_title = tr(Str::UsbBannerAndroidReadyTitle);
                        usb_desc  = tr(Str::UsbBannerAndroidReadyDesc);
                    }
                    text_at(bold_font, 86, ub_y + 13, pal.text_ink, usb_title);
                    text_at(small_font, 86, ub_y + 38, pal.text_muted, usb_desc);
                    usb_offset = 82.0f;
                }

                const float center_y = hero_y + usb_offset;
                const ImVec2 tile(origin.x + cx - 34, origin.y + center_y);
                draw->AddRectFilled(tile, ImVec2(tile.x + 68, tile.y + 68), pal.accent_blue_soft, 20.0f);
                draw->AddRect(tile, ImVec2(tile.x + 68, tile.y + 68), pal.border_glass, 20.0f);
                const ui_draw::Icon hero_ic =
                    (preferences.platform_mode == 1)   ? ui_draw::Icon::Android :
                    (preferences.connection_mode == 1) ? ui_draw::Icon::Usb     :
                                                         ui_draw::Icon::Cast;
                ui_draw::icon(draw, hero_ic, ImVec2(tile.x + 17, tile.y + 17), 34, pal.accent_blue);

                center_text(heading_font, center_y + 80, pal.text_ink, tr(Str::HeroHeading1));
                center_text(page_font,    center_y + 124, pal.text_muted, tr(Str::HeroHeading2));

                const char* hero_sub =
                    (preferences.platform_mode == 0 && preferences.connection_mode == 0) ? tr(Str::HeroSubtitleIosWifi) :
                    (preferences.platform_mode == 0 && preferences.connection_mode == 1) ? tr(Str::HeroSubtitleIosUsb) :
                    (preferences.platform_mode == 1 && preferences.connection_mode == 0) ? tr(Str::HeroSubtitleAndroidWifi) :
                                                                                           tr(Str::HeroSubtitleAndroidUsb);
                center_text(io.FontDefault, center_y + 162, pal.text_muted, hero_sub);

                // Receiver identity card OR Android Wireless Quick-Connect card
                const float card_w = std::min(500.0f, width);
                const float card_x = (stage_w - card_w) * 0.5f;
                const float card_y = center_y + 200.0f;
                const ImVec2 cp(origin.x + card_x, origin.y + card_y);
                draw->AddRectFilled(cp, ImVec2(cp.x + card_w, cp.y + 78), pal.bg_card, 14.0f);
                draw->AddRect(cp, ImVec2(cp.x + card_w, cp.y + 78), pal.border_line, 14.0f);

                if (preferences.platform_mode == 1 && preferences.connection_mode == 0) {
                    text_at(small_font, card_x + 20, card_y + 13, pal.text_muted, tr(Str::CardAndroidQuickConnect));
                    ImGui::SetCursorPos(ImVec2(card_x + 20, card_y + 35));
                    ImGui::SetNextItemWidth(card_w - 148.0f);
                    const bool enter_pressed = ImGui::InputTextWithHint(
                        "##android_ip_input", tr(Str::CardAndroidIpPlaceholder),
                        android_ip_buf.data(), android_ip_buf.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine(0, 8.0f);
                    if ((ui_draw::text_button(tr(Str::CardAndroidConnectBtn), ImVec2(100, 34), preferences.dark_mode) ||
                         enter_pressed) && android_connect_handler_) {
                        android_connect_handler_(android_ip_buf.data());
                    }
                } else {
                    text_at(small_font, card_x + 20, card_y + 14, pal.text_muted, tr(Str::CardSelectOnDevice));
                    draw->PushClipRect(cp, ImVec2(cp.x + card_w - 58, cp.y + 78), true);
                    std::string badge_line = std::string(receiver);
                    if (!idle_ip_.empty()) badge_line += "   ·   " + idle_ip_;
                    text_at(receiver_font, card_x + 20, card_y + 38, pal.text_ink, badge_line.c_str());
                    draw->PopClipRect();
                    ImGui::SetCursorPos(ImVec2(card_x + card_w - 52, card_y + 21));
                    if (ui_draw::icon_button("##rename", ui_draw::Icon::Settings,
                                             tr(Str::CardReceiverSettingsTooltip),
                                             35.0f, false, preferences.dark_mode)) {
                        ui.settings_open = true;
                    }
                }

                // 3-Step Platform/Mode-Aware Guided Cards
                const float steps_y = card_y + 98.0f;
                draw->AddLine(ImVec2(origin.x + 36, origin.y + steps_y),
                              ImVec2(origin.x + stage_w - 36, origin.y + steps_y), pal.border_line);

                const char* s_titles[3]{};
                const char* s_descs[3]{};
                if (preferences.platform_mode == 0 && preferences.connection_mode == 0) {
                    s_titles[0] = tr(Str::Step1IosWifiTitle); s_descs[0] = tr(Str::Step1IosWifiDesc);
                    s_titles[1] = tr(Str::Step2IosWifiTitle); s_descs[1] = tr(Str::Step2IosWifiDesc);
                    s_titles[2] = tr(Str::Step3IosWifiTitle); s_descs[2] = tr(Str::Step3IosWifiDesc);
                } else if (preferences.platform_mode == 0 && preferences.connection_mode == 1) {
                    s_titles[0] = tr(Str::Step1IosUsbTitle);  s_descs[0] = tr(Str::Step1IosUsbDesc);
                    s_titles[1] = tr(Str::Step2IosUsbTitle);  s_descs[1] = tr(Str::Step2IosUsbDesc);
                    s_titles[2] = tr(Str::Step3IosUsbTitle);  s_descs[2] = tr(Str::Step3IosUsbDesc);
                } else if (preferences.platform_mode == 1 && preferences.connection_mode == 0) {
                    s_titles[0] = tr(Str::Step1AndroidWifiTitle); s_descs[0] = tr(Str::Step1AndroidWifiDesc);
                    s_titles[1] = tr(Str::Step2AndroidWifiTitle); s_descs[1] = tr(Str::Step2AndroidWifiDesc);
                    s_titles[2] = tr(Str::Step3AndroidWifiTitle); s_descs[2] = tr(Str::Step3AndroidWifiDesc);
                } else {
                    s_titles[0] = tr(Str::Step1AndroidUsbTitle);  s_descs[0] = tr(Str::Step1AndroidUsbDesc);
                    s_titles[1] = tr(Str::Step2AndroidUsbTitle);  s_descs[1] = tr(Str::Step2AndroidUsbDesc);
                    s_titles[2] = tr(Str::Step3AndroidUsbTitle);  s_descs[2] = tr(Str::Step3AndroidUsbDesc);
                }

                const float col_w = (width - 32.0f) / 3.0f;
                for (int i = 0; i < 3; ++i) {
                    const float x = 36.0f + i * (col_w + 16.0f);
                    const ImVec2 card_tl(origin.x + x, origin.y + steps_y + 18.0f);
                    const ImVec2 card_br(card_tl.x + col_w, card_tl.y + 114.0f);
                    draw->AddRectFilled(card_tl, card_br, pal.bg_card, 12.0f);
                    draw->AddRect(card_tl, card_br, pal.border_line, 12.0f);

                    const ImVec2 circle(card_tl.x + 24.0f, card_tl.y + 24.0f);
                    draw->AddCircleFilled(circle, 11.0f, pal.accent_blue_soft, 24);
                    char num_str[2] = {static_cast<char>('1' + i), 0};
                    draw->AddText(bold_font, 14.0f, ImVec2(circle.x - 4.0f, circle.y - 7.5f), pal.accent_blue, num_str);
                    draw->AddText(bold_font, bold_font->FontSize, ImVec2(card_tl.x + 44.0f, card_tl.y + 14.0f),
                                  pal.text_ink, s_titles[i]);
                    draw->AddText(small_font, 14.5f, ImVec2(card_tl.x + 16.0f, card_tl.y + 46.0f),
                                  pal.text_muted, s_descs[i], nullptr, col_w - 32.0f);
                }

                const char* help_lbl = tr(Str::HelpLinkNotShowing);
                const float help_w = small_font->CalcTextSizeA(15.0f, 10000, 0, help_lbl).x;
                ImGui::SetCursorPos(ImVec2((stage_w - help_w) * 0.5f, steps_y + 144.0f));
                if (ui_draw::surface("##connection_help", ImVec2(help_w, 22)))
                    ImGui::OpenPopup("Connection help");
                const ImVec2 hp = ImGui::GetItemRectMin();
                draw->AddText(small_font, 15.0f, hp, pal.text_muted, help_lbl);
                draw->AddLine(ImVec2(hp.x, hp.y + 17), ImVec2(hp.x + help_w, hp.y + 17), pal.text_muted);

                const float help_card_width = std::min(460.0f, stage_w - 64.0f);
                ImGui::SetNextWindowSize(ImVec2(help_card_width, 0));
                ImGui::SetNextWindowPos(ImVec2(origin.x + stage_w * 0.5f, std::min(hp.y - 10.0f, fh - 20.0f)),
                                        ImGuiCond_Appearing, ImVec2(0.5f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
                if (ImGui::BeginPopup("Connection help", ImGuiWindowFlags_NoMove)) {
                    ImGui::PushFont(page_font);
                    ImGui::TextUnformatted(tr(Str::HelpPopupTitle));
                    ImGui::PopFont();
                    ImGui::SameLine(help_card_width - 59);
                    if (ui_draw::icon_button("##close_help", ui_draw::Icon::Close,
                                             tr(Str::HelpCloseTooltip), 32.0f, false, preferences.dark_mode)) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(help_card_width - 24);
                    auto help_step = [&](const char* h_title, const char* h_desc) {
                        ImGui::PushFont(bold_font); ImGui::TextUnformatted(h_title); ImGui::PopFont();
                        ImGui::PushFont(small_font); ImGui::TextDisabled("%s", h_desc); ImGui::PopFont();
                        ImGui::Dummy(ImVec2(0, 6));
                    };
                    help_step(tr(Str::HelpStep1Title), tr(Str::HelpStep1Desc));
                    help_step(tr(Str::HelpStep2Title), tr(Str::HelpStep2Desc));
                    help_step(tr(Str::HelpStep3Title), tr(Str::HelpStep3Desc));
                    ImGui::Separator();
                    ImGui::PushFont(small_font);
                    ImGui::TextDisabled("%s", tr(Str::HelpFooterNote));
                    ImGui::PopFont();
                    ImGui::PopTextWrapPos();
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar();
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        } // !no_chrome

        // ====================================================================
        // NON-INTRUSIVE SMART AUTO-RECONNECT TRANSLUCENT OVERLAY
        // ====================================================================
        if (is_reconnecting_overlay() && has_active_dev) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const char* title_msg = tr(Str::StatusAutoReconnecting);
            const char* sub_msg   = tr(Str::StatusAutoReconnectingDetail);
            const ImVec2 t_sz = bold_font->CalcTextSizeA(bold_font->FontSize, 1000.0f, 0.0f, title_msg);
            const ImVec2 s_sz = small_font->CalcTextSizeA(small_font->FontSize, 1000.0f, 0.0f, sub_msg);
            const float box_w = std::max(t_sz.x, s_sz.x) + 84.0f;
            const float box_h = 62.0f;
            const float bx = static_cast<float>(stage_x) + (static_cast<float>(stage_w) - box_w) * 0.5f;
            const float by = static_cast<float>(stage_y) + 24.0f;
            fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h),
                              IM_COL32(15, 18, 24, 224), 31.0f);
            fg->AddRect(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h),
                        IM_COL32(245, 158, 11, 180), 31.0f, 0, 1.5f);

            // Animated spinning arc
            const float t_anim = static_cast<float>(ImGui::GetTime()) * 5.0f;
            const ImVec2 sp_center(bx + 32.0f, by + box_h * 0.5f);
            fg->PathArcTo(sp_center, 12.0f, t_anim, t_anim + 4.2f, 24);
            fg->PathStroke(IM_COL32(245, 158, 11, 255), 0, 2.6f);

            fg->AddText(bold_font, bold_font->FontSize,
                        ImVec2(bx + 56.0f, by + 11.0f),
                        IM_COL32(255, 255, 255, 255), title_msg);
            fg->AddText(small_font, small_font->FontSize,
                        ImVec2(bx + 56.0f, by + 34.0f),
                        IM_COL32(200, 208, 220, 255), sub_msg);
        }

        // Screenshot saved toast notification
        if (ImGui::GetTime() < screenshot_toast_until) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const std::string toast = std::string(tr(Str::ScreenshotSavedToast)) + " (" + last_screenshot_path + ")";
            const ImVec2 tsz = small_font->CalcTextSizeA(small_font->FontSize, 900.0f, 0.0f, toast.c_str());
            const float tw = tsz.x + 36.0f;
            const float th = 36.0f;
            const float tx = static_cast<float>(stage_x) + (static_cast<float>(stage_w) - tw) * 0.5f;
            const float ty = static_cast<float>(stage_y) + 20.0f;
            fg->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), pal.bg_glass_dock, 18.0f);
            fg->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), pal.status_green, 18.0f);
            fg->AddText(small_font, small_font->FontSize, ImVec2(tx + 18.0f, ty + 9.0f), pal.text_ink, toast.c_str());
        }

        // ====================================================================
        // LIVE ACTIONS SECTION / AUTO-HIDING FLOATING CONTROL DOCK
        // ====================================================================
        // Auto-hides smoothly after 2.4s of mouse inactivity during active video
        // mirroring, and stays visible on the idle stage or whenever the mouse
        // moves / hovers near the bottom dock.
        {
            const double idle_mouse_s =
                std::chrono::duration<double>(now - last_mouse_move_time).count();
            const float dock_w = std::min(820.0f, static_cast<float>(stage_w) - 32.0f);
            const float dock_h = 52.0f;
            const float dock_x = static_cast<float>(stage_x) + (static_cast<float>(stage_w) - dock_w) * 0.5f;
            const float dock_y = static_cast<float>(stage_y + stage_h) - dock_h - 14.0f;
            const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
            const bool mouse_near_dock =
                (mouse_pos.x >= dock_x - 20.0f && mouse_pos.x <= dock_x + dock_w + 20.0f &&
                 mouse_pos.y >= dock_y - 36.0f && mouse_pos.y <= dock_y + dock_h + 20.0f);
            const bool should_show_dock =
                !ui.settings_open &&
                stage_w >= 540 && stage_h >= 240 &&
                (!show_video || idle_mouse_s < 2.4 || mouse_near_dock);

            if (should_show_dock) {
                ImGui::SetNextWindowPos(ImVec2(dock_x, dock_y));
                ImGui::SetNextWindowSize(ImVec2(dock_w, dock_h));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 9));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 26.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_glass_dock));
                if (ImGui::Begin("##live_control_dock", nullptr,
                                 kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
                    auto* dlist = ImGui::GetWindowDrawList();
                    const ImVec2 wp = ImGui::GetWindowPos();
                    const ImVec2 ws = ImGui::GetWindowSize();
                    dlist->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                                   pal.border_glass, 26.0f, 0, 1.2f);

                    // 1. Connection Mode Toggle (Wireless Wi-Fi vs. Wired USB)
                    const bool is_usb_mode = (preferences.connection_mode == 1);
                    if (ui_draw::pill_button(
                            "##dock_mode",
                            is_usb_mode ? tr(Str::DockModeUsb) : tr(Str::DockModeWifi),
                            is_usb_mode ? ui_draw::Icon::Usb : ui_draw::Icon::Wifi,
                            is_usb_mode, tr(Str::DockTooltipMode),
                            small_font, preferences.dark_mode, 34.0f)) {
                        preferences.connection_mode = is_usb_mode ? 0 : 1;
                    }
                    ImGui::SameLine(0, 6.0f);

                    // 2. Platform Selector (iOS AirPlay vs. Android Mirroring)
                    const bool is_android_mode = (preferences.platform_mode == 1);
                    if (ui_draw::pill_button(
                            "##dock_platform",
                            is_android_mode ? tr(Str::DockPlatformAndroid) : tr(Str::DockPlatformIos),
                            is_android_mode ? ui_draw::Icon::Android : ui_draw::Icon::Phone,
                            is_android_mode, tr(Str::DockTooltipPlatform),
                            small_font, preferences.dark_mode, 34.0f)) {
                        preferences.platform_mode = is_android_mode ? 0 : 1;
                    }
                    ImGui::SameLine(0, 8.0f);

                    // 3. Manual Reconnect / Refresh Stream
                    if (ui_draw::icon_button("##dock_reconnect", ui_draw::Icon::Refresh,
                                             tr(Str::DockTooltipReconnect), 34.0f, false, preferences.dark_mode)) {
                        if (reconnect_handler_) reconnect_handler_();
                    }
                    ImGui::SameLine(0, 4.0f);

                    // 4. Auto-Rotate / Aspect-Ratio Lock (Portrait & Landscape dynamic resizing)
                    if (ui_draw::icon_button("##dock_rotate", ui_draw::Icon::Rotate,
                                             tr(Str::DockTooltipRotate), 34.0f, false, preferences.dark_mode)) {
                        if (live_settings_) {
                            int cur_rot = live_settings_->video_rotation.load();
                            cur_rot = (cur_rot >= 3) ? -1 : (cur_rot + 1);
                            live_settings_->video_rotation.store(cur_rot);
                            snap_preserving_area(true);
                        }
                    }
                    ImGui::SameLine(0, 4.0f);

                    // 5. Screen Recording & Instant Screenshot buttons
                    const bool is_rec = recorder_ && recorder_->status().recording;
                    if (ui_draw::pill_button(
                            "##dock_record",
                            is_rec ? tr(Str::BtnStopRecording) : tr(Str::BtnRecord),
                            ui_draw::Icon::Record, is_rec,
                            tr(Str::DockTooltipRecord),
                            small_font, preferences.dark_mode, 34.0f)) {
                        toggle_recording();
                    }
                    ImGui::SameLine(0, 4.0f);
                    if (ui_draw::icon_button("##dock_screenshot", ui_draw::Icon::Camera,
                                             tr(Str::DockTooltipScreenshot), 34.0f, false, preferences.dark_mode)) {
                        screenshot_requested = true;
                    }
                    ImGui::SameLine(0, 4.0f);
                    if (ui_draw::icon_button("##dock_fullscreen", ui_draw::Icon::Fullscreen,
                                             tr(Str::DockTooltipFullscreen), 34.0f, false, preferences.dark_mode)) {
                        toggle_fullscreen();
                    }
                    ImGui::SameLine(0, 10.0f);

                    // 6. 1:1 Synchronized Master Volume Slider & Mute toggle
                    if (ui_draw::icon_button(
                            "##dock_mute",
                            preferences.muted ? ui_draw::Icon::VolumeMute : ui_draw::Icon::Volume,
                            tr(Str::DockTooltipMute), 34.0f, false, preferences.dark_mode)) {
                        preferences.muted = !preferences.muted;
                        ap::audio::MasterVolume::instance().set(preferences.master_volume, preferences.muted);
                    }
                    ImGui::SameLine(0, 4.0f);
                    ImGui::SetNextItemWidth(std::clamp(dock_w - 575.0f, 70.0f, 130.0f));
                    int vol_pct = preferences.master_volume;
                    if (ImGui::SliderInt("##dock_vol", &vol_pct, 0, 100, "%d%%")) {
                        preferences.master_volume = std::clamp(vol_pct, 0, 100);
                        if (preferences.master_volume > 0 && preferences.muted) {
                            preferences.muted = false;
                        }
                        ap::audio::MasterVolume::instance().set(preferences.master_volume, preferences.muted);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(Str::DockTooltipVolume));

                    // 7. Quick EN / VI Language Switcher
                    ImGui::SameLine(0, 8.0f);
                    const bool vi_now = (ap::i18n::current_language() == ap::i18n::Language::Vietnamese);
                    if (ui_draw::pill_button(
                            "##dock_lang", vi_now ? "VI" : "EN",
                            ui_draw::Icon::Globe, vi_now,
                            tr(Str::BtnLangSwitchTooltip),
                            small_font, preferences.dark_mode, 34.0f)) {
                        ap::i18n::LocalizationManager::instance().toggle_language();
                        preferences.language = static_cast<int>(ap::i18n::current_language());
                        idle_dirty_ = true;
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
            }
        }

        // ---- Settings Drawer (Bilingual EN / VI) ----
        if (!no_chrome && ui.settings_open) {
            ImGui::SetNextWindowPos(ImVec2(0, toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(fw - opts_bar_w, fh - toolbar_h - status_bar_h));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.07f, 0.10f, 0.40f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##settings_shade", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar);
            if (ui_draw::surface("##dismiss_settings", ImGui::GetContentRegionAvail()))
                ui.settings_open = false;
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            ImGui::SetNextWindowPos(ImVec2(fw - opts_bar_w, toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(opts_bar_w, fh - toolbar_h - status_bar_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_sidebar));
            ImGui::Begin("##settings_drawer", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPos(ImVec2(0, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12);
            if (ImGui::BeginChild("##options_panel", ImVec2(opts_bar_w - 8, fh - toolbar_h - status_bar_h - 16), false,
                                  kPanelFlags | ImGuiWindowFlags_AlwaysUseWindowPadding)) {
                ImGui::PushFont(page_font);
                ImGui::TextUnformatted(tr(Str::SettingsTitle));
                ImGui::PopFont();
                ImGui::SameLine(opts_bar_w - 65);
                if (ui_draw::icon_button("##close_settings", ui_draw::Icon::Close,
                                         tr(Str::SettingsCloseTooltip), 34.0f, false, preferences.dark_mode)) {
                    ui.settings_open = false;
                }
                ImGui::Separator();
                ImGui::PushTextWrapPos(0.0f);

                auto settings_section = [&](const char* label, ImGuiTreeNodeFlags flags = 0) {
                    ImGui::PushFont(bold_font);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
                    const bool open = ImGui::CollapsingHeader(label, flags);
                    ImGui::PopStyleVar();
                    ImGui::PopFont();
                    return open;
                };

                // Appearance & Language Section
                if (settings_section(tr(Str::SettingsSectionLanguageTheme), ImGuiTreeNodeFlags_DefaultOpen)) {
                    const char* langs[] = {"English (EN)", "Tiếng Việt (VI)"};
                    int cur_lang = static_cast<int>(ap::i18n::current_language());
                    if (ImGui::Combo(tr(Str::SettingsLanguageLabel), &cur_lang, langs, 2)) {
                        ap::i18n::LocalizationManager::instance().set_language(
                            static_cast<ap::i18n::Language>(cur_lang));
                        preferences.language = cur_lang;
                        idle_dirty_ = true;
                    }
                    const char* themes[] = {
                        tr(Str::SettingsThemeDarkOption),
                        tr(Str::SettingsThemeLightOption)
                    };
                    int theme_idx = preferences.dark_mode ? 0 : 1;
                    if (ImGui::Combo(tr(Str::SettingsThemeLabel), &theme_idx, themes, 2)) {
                        preferences.dark_mode = (theme_idx == 0);
                    }
                    ImGui::Checkbox(tr(Str::SettingsDeviceFrameLabel), &preferences.device_frame);
                }

                // Receiver Broadcast Name
                if (live_settings_ && settings_section(tr(Str::SettingsSectionReceiverName),
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool enter = ImGui::InputText("##receiver_name",
                        receiver_name_buf.data(), receiver_name_buf.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    const auto name = ap::airplay::normalize_receiver_name(receiver_name_buf.data());
                    const bool changed = name != live_settings_->receiver_name();
                    ImGui::BeginDisabled(name.empty() || !changed);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal.accent_blue));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    const bool apply = ImGui::Button(tr(Str::SettingsApplyName));
                    ImGui::PopStyleColor(2);
                    ImGui::EndDisabled();
                    if ((apply || enter) && !name.empty() && changed) {
                        live_settings_->set_receiver_name(name);
                        std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s", name.c_str());
                    }
                    if (name.empty())
                        ImGui::TextDisabled("%s", tr(Str::SettingsNameInvalidHint));
                    else
                        ImGui::TextDisabled("%s", tr(Str::SettingsNameSavedHint));
                }

                // Mirror Resolution
                if (settings_section(tr(Str::SettingsSectionResolution), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        struct Preset { const char* label; int w; int h; };
                        static const Preset presets[] = {
                            {"1920x1080  (Full HD)",      1920, 1080},
                            {"2560x1440  (QHD, default)", 2560, 1440},
                            {"1920x1920  (Square 1080p)", 1920, 1920},
                            {"2560x2560  (Square QHD)",   2560, 2560},
                            {"2868x2868  (iPhone Pro)",   2868, 2868},
                            {"3840x2160  (4K UHD)",       3840, 2160},
                            {"3840x3840  (4K Square)",    3840, 3840},
                        };
                        int cur_w = live_settings_->mirror_width.load();
                        int cur_h = live_settings_->mirror_height.load();
                        int sel = -1;
                        for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                            if (presets[i].w == cur_w && presets[i].h == cur_h) {
                                sel = i; break;
                            }
                        }
                        if (sel < 0) custom_resolution = true;

                        char preview_buf[48];
                        const char* preview;
                        if (custom_resolution) {
                            std::snprintf(preview_buf, sizeof(preview_buf), "Custom: %dx%d", cur_w, cur_h);
                            preview = preview_buf;
                        } else {
                            preview = presets[sel].label;
                        }

                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##mirror_res", preview)) {
                            for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                                const bool is_sel = (!custom_resolution && sel == i);
                                if (ImGui::Selectable(presets[i].label, is_sel)) {
                                    custom_resolution = false;
                                    live_settings_->mirror_width.store(presets[i].w);
                                    live_settings_->mirror_height.store(presets[i].h);
                                }
                                if (is_sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::Separator();
                            if (ImGui::Selectable(tr(Str::SettingsCustomRes), custom_resolution)) {
                                custom_resolution = true;
                            }
                            ImGui::EndCombo();
                        }

                        if (custom_resolution) {
                            static int  width_buf    = 0;
                            static int  height_buf   = 0;
                            static bool width_active = false;
                            static bool height_active= false;
                            if (!width_active)  width_buf  = cur_w;
                            if (!height_active) height_buf = cur_h;

                            ImGui::SetNextItemWidth(90);
                            ImGui::InputInt(tr(Str::SettingsWidth), &width_buf, 0, 0);
                            width_active = ImGui::IsItemActive();
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(90);
                            ImGui::InputInt(tr(Str::SettingsHeight), &height_buf, 0, 0);
                            height_active = ImGui::IsItemActive();

                            if (width_buf >= 320 && width_buf <= 7680 && width_buf != cur_w)
                                live_settings_->mirror_width.store(width_buf);
                            if (height_buf >= 240 && height_buf <= 4320 && height_buf != cur_h)
                                live_settings_->mirror_height.store(height_buf);
                            ImGui::TextDisabled("%s", tr(Str::SettingsResRangeHint));
                        }
                        ImGui::TextDisabled("%s", tr(Str::SettingsResMaxHint));
                        if (video_tex_w > 0 && video_tex_h > 0)
                            ImGui::TextDisabled(tr(Str::SettingsResReceivedFmt), video_tex_w, video_tex_h);
                        ImGui::TextDisabled("%s", tr(Str::SettingsAppliesNextConn));
                    }
                }

                // Hardware Video Decoder & VSync
                if (settings_section(tr(Str::SettingsSectionDecoder), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        bool hw = live_settings_->mirror_hwaccel.load();
                        if (ImGui::Checkbox(tr(Str::SettingsUseGpu), &hw)) {
                            live_settings_->mirror_hwaccel.store(hw);
                        }
                        ImGui::TextDisabled("%s", hw ? tr(Str::SettingsGpuHint) : tr(Str::SettingsCpuHint));
                        ImGui::TextDisabled("%s", tr(Str::SettingsAppliesNextConn));
                        ImGui::Separator();
                        bool vsync = live_settings_->vsync_enabled.load();
                        if (ImGui::Checkbox(tr(Str::SettingsVsync), &vsync)) {
                            live_settings_->vsync_enabled.store(vsync);
                        }
                        ImGui::TextDisabled("%s", vsync ? tr(Str::SettingsVsyncOnHint) : tr(Str::SettingsVsyncOffHint));
                        ImGui::TextDisabled("%s", tr(Str::SettingsAppliesLive));
                    }
                }

                // Smart Auto-Reconnect & Watchdog
                if (live_settings_ && settings_section(tr(Str::SettingsSectionStability))) {
                    int recover_s = live_settings_->auto_recover_seconds.load();
                    if (ImGui::SliderInt(tr(Str::SettingsAutoRecoverLabel), &recover_s, 0, 30, "%d s")) {
                        live_settings_->auto_recover_seconds.store(recover_s);
                    }
                    ImGui::TextDisabled("%s", tr(Str::SettingsAutoRecoverHint));
                }

                // Screen Recording
                if (settings_section(tr(Str::SettingsSectionRecording))) {
                    const auto rec = recorder_->status();
                    ImGui::TextUnformatted(tr(Str::SettingsFolder));
#if defined(_WIN32)
                    ImGui::SetNextItemWidth(-92.0f);
#endif
                    ImGui::InputText("##recording_folder", recording_dir.data(), recording_dir.size());
#if defined(_WIN32)
                    ImGui::SameLine();
                    if (ImGui::Button(tr(Str::SettingsBrowse), ImVec2(-1.0f, 0.0f))) {
                        std::string selected_folder;
                        if (browse_recording_folder(window, recording_dir.data(), selected_folder)) {
                            std::snprintf(recording_dir.data(), recording_dir.size(), "%s", selected_folder.c_str());
                        }
                    }
#endif
                    const char* encoder_modes[] = {
                        "Auto (GPU preferred)", "GPU only", "CPU only"
                    };
                    const char* video_codecs[] = {
                        "H.264 (AVC)", "H.265 / HEVC"
                    };
                    int video_codec = static_cast<int>(recording_options.codec);
                    if (ImGui::Combo(tr(Str::SettingsVideoCodec), &video_codec, video_codecs, 2)) {
                        recording_options.codec = static_cast<VideoRecorder::VideoCodec>(video_codec);
                    }
                    int encoder_mode = static_cast<int>(recording_options.encoder);
                    if (ImGui::Combo(tr(Str::SettingsEncoder), &encoder_mode, encoder_modes, 3)) {
                        recording_options.encoder = static_cast<VideoRecorder::EncoderMode>(encoder_mode);
                    }
                    ImGui::SliderInt(tr(Str::SettingsBitrateMbps), &recording_options.bitrate_mbps, 2, 40);
                    ImGui::TextDisabled("%s", tr(Str::SettingsBitrateHint));
                    if (rec.recording) {
                        const int64_t seconds = rec.elapsed_ms / 1000;
                        ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.32f, 1.0f),
                                           "REC  %02lld:%02lld",
                                           static_cast<long long>(seconds / 60),
                                           static_cast<long long>(seconds % 60));
                        if (ImGui::Button(tr(Str::BtnStopRecording), ImVec2(-1, 0)))
                            toggle_recording();
                    } else {
                        ImGui::TextDisabled("%s", tr(Str::SettingsRecMp4Hint));
                        if (ImGui::Button(tr(Str::SettingsStartRecording), ImVec2(-1, 0)))
                            toggle_recording();
                    }
                    if (!rec.output_path.empty())
                        ImGui::TextWrapped("File: %s", rec.output_path.c_str());
                    if (!rec.error.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", rec.error.c_str());
                }

                // Advanced & Orientation
                ImGui::Separator();
                if (settings_section(tr(Str::SettingsSectionAdvanced))) {
                    ImGui::Checkbox(tr(Str::SettingsShowDiagnostics), &ui.diagnostics);
                    if (settings_section(tr(Str::SettingsSectionRotation))) {
                        if (live_settings_) {
                            int selected = live_settings_->video_rotation.load() + 1;
                            ImGui::SetNextItemWidth(-1.0f);
                            if (ImGui::Combo("##video_rotation", &selected, rotation_labels, 5))
                                live_settings_->video_rotation.store(selected - 1);
                            ImGui::TextDisabled("%s", tr(Str::SettingsRotationHint));
                        }
                    }
                    if (settings_section(tr(Str::SettingsSectionFramerate))) {
                        if (live_settings_) {
                            int cap = live_settings_->max_fps.load();
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderInt(tr(Str::SettingsFpsCap), &cap, 1, 60, "%d fps")) {
                                live_settings_->max_fps.store(cap);
                                live_settings_->refresh_rate.store(cap);
                            }
                            ImGui::TextDisabled("%s", tr(Str::SettingsFpsHint));
                        }
                    }
                    if (settings_section(tr(Str::SettingsSectionCompression))) {
                        if (live_settings_) {
                            bool hevc = live_settings_->hevc_enabled.load();
                            if (ImGui::Checkbox(tr(Str::SettingsAllowHevc), &hevc)) {
                                live_settings_->hevc_enabled.store(hevc);
                            }
                            ImGui::TextDisabled("%s", hevc ? tr(Str::SettingsHevcOnHint) : tr(Str::SettingsHevcOffHint));
                        }
                    }
                    if (settings_section(tr(Str::SettingsSectionNetwork))) {
                        ImGui::TextDisabled("AirPlay 2 RTSP   :7000");
                        ImGui::TextDisabled("Android Mirror   :7001");
                        ImGui::TextDisabled("HLS Local Proxy  :7100");
                        ImGui::TextDisabled("Bonjour / mDNS   Active");
                    }
                    if (settings_section(tr(Str::SettingsSectionShortcuts))) {
                        ImGui::BulletText("%s", tr(Str::SettingsShortcutFullscreen));
                        ImGui::BulletText("%s", tr(Str::SettingsShortcutHideUi));
                        ImGui::BulletText("%s", tr(Str::SettingsShortcutRecord));
                        ImGui::BulletText("%s", tr(Str::SettingsShortcutScreenshot));
                        ImGui::BulletText("%s", tr(Str::SettingsShortcutEsc));
                    }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr(Str::SettingsResetHint));
                ImGui::BeginDisabled(!live_settings_);
                if (ui_draw::text_button(tr(Str::SettingsResetBtn), ImVec2(-1, 36), preferences.dark_mode))
                    reset_all_settings();
                ImGui::EndDisabled();
                if (ImGui::GetTime() < settings_reset_notice_until)
                    ImGui::TextDisabled("%s", tr(Str::SettingsResetDone));
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // ---- Bottom Telemetry Status Bar ----
        if (!no_chrome && ui.diagnostics) {
            ImGui::SetNextWindowPos(ImVec2(0.0f, fh - status_bar_h));
            ImGui::SetNextWindowSize(ImVec2(fw, status_bar_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_sidebar));
            if (ImGui::Begin("##status_bar", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
                const ImGuiIO& io_ui = ImGui::GetIO();
                const bool playing = playing_.load(std::memory_order_relaxed);
                const bool has_video =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_video_frame_time).count() < 1500;
                const int      vid_w = video_w_seen_.load(std::memory_order_relaxed);
                const int      vid_h = video_h_seen_.load(std::memory_order_relaxed);
                const float    vid_fps = video_fps_ema_.load(std::memory_order_relaxed);
                const uint64_t total = frames_total_.load(std::memory_order_relaxed);
                if (ImGui::BeginTable("##diagnostics_metrics", diagnostics_columns,
                                      ImGuiTableFlags_SizingStretchSame)) {
                    ImGui::TableNextColumn();
                    const char* state_label =
                        is_reconnecting_overlay() ? tr(Str::DiagStateReconnecting) :
                        has_active_dev
                            ? (has_video ? (playing ? tr(Str::DiagStateStreaming) : tr(Str::DiagStatePaused))
                                         : tr(Str::DiagStateConnected))
                            : tr(Str::DiagStateIdle);
                    ImGui::Text("State: %s", state_label);
                    ImGui::TableNextColumn();
                    if (vid_w > 0 && vid_h > 0) {
                        ImGui::Text("Video: %dx%d @ %.1f", vid_w, vid_h, has_video ? vid_fps : 0.0f);
                    } else {
                        ImGui::TextDisabled("Video: -");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("Frames: %llu", static_cast<unsigned long long>(total));
                    ImGui::TableNextColumn();
                    ImGui::Text("Stage: %dx%d", stage_w, stage_h);
                    ImGui::TableNextColumn();
                    ImGui::Text("Renderer: %.0f fps", io_ui.Framerate);
                    ImGui::TableNextColumn();
                    const float mbps = payload_mbps_ema_.load(std::memory_order_relaxed);
                    ImGui::Text("BW: %.1f Mbps", has_video ? mbps : 0.0f);
                    ImGui::TableNextColumn();
                    const float lat_ms = pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
                    ImGui::Text("Lat: %.1f ms", has_video ? lat_ms : 0.0f);
                    ImGui::TableNextColumn();
                    ImGui::Text("Auto-Recovers: %llu",
                                static_cast<unsigned long long>(auto_recovery_count()));
                    ImGui::EndTable();
                }
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // Pulsing ring on chromeless idle screen
        {
            const bool video_streaming_idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            if (no_chrome && !video_streaming_idle && !has_active_dev &&
                stage_w > 0 && stage_h > 0) {
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                const float cx = static_cast<float>(stage_x) + stage_w * 0.5f;
                const float cy = static_cast<float>(stage_y) + stage_h * 0.30f;
                const float t  = static_cast<float>(ImGui::GetTime());
                for (int i = 0; i < 2; ++i) {
                    const float speed  = (i == 0) ? 1.6f : 2.4f;
                    const float phase  = (i == 0) ? 0.0f : 1.05f;
                    const float pulse  = 0.5f + 0.5f * std::sin(t * speed + phase);
                    const float r_min  = (i == 0) ? 32.0f : 18.0f;
                    const float r_max  = (i == 0) ? 58.0f : 32.0f;
                    const float radius = r_min + (r_max - r_min) * pulse;
                    const int   alpha  = static_cast<int>((1.0f - pulse) * 110.0f);
                    bg->AddCircle(ImVec2(cx, cy), radius,
                                  IM_COL32(59, 130, 246, alpha), 64, 2.0f);
                }
                bg->AddCircleFilled(ImVec2(cx, cy), 8.0f,
                                    IM_COL32(59, 130, 246, 220));
            }
        }

        publish_preferences();
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        const auto present_t0 = std::chrono::steady_clock::now();
        SDL_RenderPresent(renderer);
        {
            const auto present_t1 = std::chrono::steady_clock::now();
            const auto present_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                present_t1 - present_t0).count();
            if (present_ms > 250) {
                ++slow_present_total;
                if (slow_present_total <= 3 || slow_present_total % 100 == 0)
                    LOG_WARN << "VideoRenderer: SDL_RenderPresent took " << present_ms
                             << " ms (count=" << slow_present_total << ')';
                if (vsync_active && ++slow_present_streak >= 3) {
                    vsync_relief = true;
                    vsync_relief_since = present_t1;
                    slow_present_streak = 0;
                    LOG_WARN << "VideoRenderer: presentation is blocking — VSYNC off temporarily";
                }
            } else {
                slow_present_streak = 0;
            }
            if (vsync_relief &&
                present_t1 - vsync_relief_since > std::chrono::seconds(15)) {
                vsync_relief = false;
                LOG_INFO << "VideoRenderer: restoring VSYNC setting";
            }
        }

        if ((have_frame || have_avf) && frame_origin_ns_local > 0) {
            const int64_t now_ns2 =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const float lat_ms =
                static_cast<float>((now_ns2 - frame_origin_ns_local) / 1e6);
            const float prev_ema =
                pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? lat_ms
                                  : prev_ema * 0.90f + lat_ms * 0.10f;
            pipeline_latency_ms_ema_.store(ema, std::memory_order_relaxed);
        }
    }

    desktop_controls.stop();
    if (keep_awake_on) ap::platform::set_keep_awake(false);
    if (recorder_) recorder_->stop();
    publish_preferences();
    if (title_tex)     SDL_DestroyTexture(title_tex);
    if (artist_tex)    SDL_DestroyTexture(artist_tex);
    if (album_tex)     SDL_DestroyTexture(album_tex);
    if (idle_name_tex) SDL_DestroyTexture(idle_name_tex);
    if (idle_ip_tex)   SDL_DestroyTexture(idle_ip_tex);
    if (idle_msg_tex)  SDL_DestroyTexture(idle_msg_tex);
    gpu_video.reset_target();
    if (video_tex)     SDL_DestroyTexture(video_tex);
    if (cover_tex)     SDL_DestroyTexture(cover_tex);
    for (TTF_Font* f : fonts_big)   if (f) TTF_CloseFont(f);
    for (TTF_Font* f : fonts_small) if (f) TTF_CloseFont(f);
    TTF_Quit();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
