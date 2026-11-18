#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace ap {
namespace {

std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

// Off by default. The CLI (--log / --verbose) flips this on.
std::atomic<bool> g_log_enabled{false};

const char* level_tag(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Notice: return "INFO ";   // same text as before the log file existed
    }
    return "?";
}

std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return os.str();
}

// ---- persistent log file ---------------------------------------------------
// Small, bounded and always on: Warn/Error/Notice lines only. One rotation
// (airplay.log -> airplay.log.1) keeps the total under ~2 MiB.
constexpr std::uintmax_t kLogFileLimit = 1024 * 1024;

struct FileSink {
    std::filesystem::path path;
    std::ofstream         out;
    std::uintmax_t        written = 0;
    bool                  configured = false;
    bool                  failed = false;
};

FileSink& sink() {
    static FileSink s;
    return s;
}

std::string date_time_stamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return os.str();
}

std::filesystem::path default_log_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"AirPlay-Windows" / L"airplay.log";
#else
    const char* env = std::getenv("AIRPLAY_LOG_FILE");
    return env ? std::filesystem::path(env) : std::filesystem::path();
#endif
}

// Caller holds log_mutex().
bool open_sink(FileSink& s) {
    if (s.failed) return false;
    if (s.out.is_open()) return true;
    if (!s.configured) {
        s.path = default_log_path();
        s.configured = true;
    }
    if (s.path.empty()) { s.failed = true; return false; }
    std::error_code ec;
    if (s.path.has_parent_path())
        std::filesystem::create_directories(s.path.parent_path(), ec);
    s.written = std::filesystem::exists(s.path, ec) ? std::filesystem::file_size(s.path, ec) : 0;
    if (ec) s.written = 0;
    s.out.open(s.path, std::ios::app);
    if (!s.out.is_open()) { s.failed = true; return false; }
    s.out << date_time_stamp() << "  ---- log opened ----\n";
    s.out.flush();
    return true;
}

void write_sink(LogLevel level, const std::string& msg) {
    if (level != LogLevel::Warn && level != LogLevel::Error &&
        level != LogLevel::Notice) return;
    FileSink& s = sink();
    if (!open_sink(s)) return;
    if (s.written > kLogFileLimit) {
        s.out.close();
        std::error_code ec;
        std::filesystem::path old = s.path;
        old += ".1";
        std::filesystem::remove(old, ec);
        std::filesystem::rename(s.path, old, ec);
        s.written = 0;
        s.out.open(s.path, std::ios::trunc);
        if (!s.out.is_open()) { s.failed = true; return; }
    }
    std::ostringstream line;
    line << date_time_stamp() << "  " << level_tag(level) << "  " << msg << '\n';
    const std::string text = line.str();
    s.out << text;
    s.out.flush();
    s.written += text.size();
}

} // namespace

void log_write(LogLevel level, const std::string& msg) {
    const bool console = g_log_enabled.load(std::memory_order_relaxed);
    const bool to_file = level == LogLevel::Warn || level == LogLevel::Error ||
                         level == LogLevel::Notice;
    if (!console && !to_file) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    if (console) {
        std::fprintf(stderr, "[%s] %s  %s\n",
                     timestamp().c_str(), level_tag(level), msg.c_str());
        std::fflush(stderr);
    }
    if (to_file) write_sink(level, msg);
}

void set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(log_mutex());
    FileSink& s = sink();
    if (s.out.is_open()) s.out.close();
    s.path = path;
    s.configured = true;
    s.failed = false;
}

void set_log_enabled(bool on) {
    g_log_enabled.store(on, std::memory_order_relaxed);
}

bool is_log_enabled() {
    return g_log_enabled.load(std::memory_order_relaxed);
}

} // namespace ap
