#include "usb/usb_supervisor.h"
#include "android/android_mirror.h"
#include "log.h"
#include "net/socket.h"
#include "usb/pair_record.h"
#include "video/video_renderer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <initguid.h>      // must precede usbiodef.h to instantiate GUIDs
    #include <usbiodef.h>      // GUID_DEVINTERFACE_USB_DEVICE
    #include <setupapi.h>
    #pragma comment(lib, "setupapi.lib")
#endif

namespace ap::usb {

#if defined(_WIN32)

namespace {

constexpr uint16_t kAppleVendorId = 0x05AC;
constexpr auto     kPollInterval  = std::chrono::seconds(1);

// Common Android OEM USB Vendor IDs (Google, Samsung, Xiaomi, Sony, OnePlus,
// OPPO, Vivo, Huawei, Motorola, ASUS, LG, HTC, Realme/BBK, Lenovo).
constexpr uint16_t kAndroidVendorIds[] = {
    0x18D1, 0x04E8, 0x2717, 0x0FCE, 0x2A70, 0x22D9, 0x2D95,
    0x12D1, 0x22B8, 0x0B05, 0x1004, 0x0BB4, 0x17EF
};

bool is_android_vid(uint16_t vid) {
    for (uint16_t candidate : kAndroidVendorIds) {
        if (vid == candidate) return true;
    }
    return false;
}

struct DiscoveredUsbDevice {
    bool        is_apple = false;
    std::string serial;
    std::string friendly_name;
    uint16_t    vid = 0;
    uint16_t    pid = 0;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

bool parse_usb_instance_id(const std::wstring& w_id,
                           uint16_t& vid_out, uint16_t& pid_out,
                           std::string& serial_out) {
    std::string id;
    id.reserve(w_id.size());
    for (auto wc : w_id) id.push_back(static_cast<char>(wc));

    auto vidpos = id.find("VID_");
    auto pidpos = id.find("PID_");
    if (vidpos == std::string::npos || pidpos == std::string::npos) return false;
    if (vidpos + 8 > id.size() || pidpos + 8 > id.size())            return false;

    unsigned long vid = 0, pidv = 0;
    try {
        vid  = std::stoul(id.substr(vidpos + 4, 4), nullptr, 16);
        pidv = std::stoul(id.substr(pidpos + 4, 4), nullptr, 16);
    } catch (...) { return false; }

    const uint16_t v16 = static_cast<uint16_t>(vid);
    if (v16 != kAppleVendorId && !is_android_vid(v16)) return false;

    auto last_slash = id.find_last_of("\\/");
    if (last_slash == std::string::npos || last_slash + 1 >= id.size()) return false;
    serial_out = id.substr(last_slash + 1);
    if (serial_out.find('&') != std::string::npos || serial_out.empty()) return false;

    vid_out = v16;
    pid_out = static_cast<uint16_t>(pidv);
    return true;
}

std::string read_friendly_name(HDEVINFO devinfo, SP_DEVINFO_DATA& data) {
    wchar_t buf[256] = {};
    DWORD   needed   = 0;
    DWORD   reg_type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(
            devinfo, &data, SPDRP_FRIENDLYNAME, &reg_type,
            reinterpret_cast<PBYTE>(buf), sizeof(buf), &needed)) {
        if (!SetupDiGetDeviceRegistryPropertyW(
                devinfo, &data, SPDRP_DEVICEDESC, &reg_type,
                reinterpret_cast<PBYTE>(buf), sizeof(buf), &needed)) {
            return {};
        }
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::vector<DiscoveredUsbDevice> enumerate_mobile_usb_devices() {
    std::vector<DiscoveredUsbDevice> devices;
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE,
                                      nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) return devices;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &data); ++i) {
        wchar_t inst_id[256] = {};
        if (!SetupDiGetDeviceInstanceIdW(h, &data, inst_id,
                                         ARRAYSIZE(inst_id), nullptr)) {
            continue;
        }
        DiscoveredUsbDevice d{};
        if (!parse_usb_instance_id(inst_id, d.vid, d.pid, d.serial)) continue;
        d.is_apple = (d.vid == kAppleVendorId);
        // Filter out Apple USB keyboards/mice/trackpads (iOS devices have PIDs in 0x1290..0x12AF)
        if (d.is_apple && (d.pid < 0x1290 || d.pid > 0x12BF)) continue;
        d.friendly_name = read_friendly_name(h, data);
        if (d.friendly_name.empty()) {
            d.friendly_name = d.is_apple ? "Apple iPhone / iPad" : "Android USB Device";
        }
        devices.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(h);
    return devices;
}

struct AdbDeviceEntry {
    std::string serial;
    std::string state; // "device", "unauthorized", "offline"
    std::string model;
};

// Query local ADB daemon (127.0.0.1:5037) with "host:devices-l" to discover
// USB-connected Android phones and their authorization status.
std::vector<AdbDeviceEntry> query_adb_usb_devices() {
    std::vector<AdbDeviceEntry> out;
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return out;
    DWORD tmo = 350;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(5037);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ap::net::close_socket(s);
        return out;
    }
    static constexpr const char kCmd[] = "000Ehost:devices-l";
    if (ap::net::send_all(s, kCmd, sizeof(kCmd) - 1) < 0) {
        ap::net::close_socket(s);
        return out;
    }
    char hdr[8] = {};
    int got = 0;
    while (got < 8) {
        int n = ::recv(s, hdr + got, 8 - got, 0);
        if (n <= 0) break;
        got += n;
    }
    if (got < 8 || std::memcmp(hdr, "OKAY", 4) != 0) {
        ap::net::close_socket(s);
        return out;
    }
    char len_hex[5] = {hdr[4], hdr[5], hdr[6], hdr[7], 0};
    unsigned plen = 0;
    if (std::sscanf(len_hex, "%04x", &plen) != 1 || plen == 0 || plen > 32768) {
        ap::net::close_socket(s);
        return out;
    }
    std::string body(plen, '\0');
    got = 0;
    while (got < static_cast<int>(plen)) {
        int n = ::recv(s, body.data() + got, static_cast<int>(plen) - got, 0);
        if (n <= 0) break;
        got += n;
    }
    ap::net::close_socket(s);

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream ls(line);
        AdbDeviceEntry entry;
        if (!(ls >> entry.serial >> entry.state)) continue;
        // Skip wireless TCP/IP ADB entries (they contain ':')
        if (entry.serial.find(':') != std::string::npos) continue;
        std::string token;
        while (ls >> token) {
            if (token.rfind("model:", 0) == 0) {
                entry.model = token.substr(6);
            }
        }
        if (entry.model.empty()) entry.model = "Android Phone";
        out.push_back(std::move(entry));
    }
    return out;
}

// Probe usbmuxd (127.0.0.1:27015) to verify that the Apple Mobile Device USB
// multiplexer is reachable for low-latency USB tunneling.
bool probe_usbmuxd_ready() {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return false;
    DWORD tmo = 250;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(27015);
    const bool ok = (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ap::net::close_socket(s);
    return ok;
}

} // namespace

struct UsbSupervisor::Impl {
    std::atomic<bool>                 running{false};
    std::thread                       thread;
    std::set<std::string>             known_keys;
    std::set<std::string>             launched_android_serials;
    mutable std::mutex                status_mtx;
    UsbStatusSnapshot                 current_status;
    ap::video::VideoRenderer*         renderer = nullptr;
    ap::android::AndroidMirrorServer* android_server = nullptr;

    void update_status(const UsbStatusSnapshot& snap) {
        {
            std::lock_guard<std::mutex> lock(status_mtx);
            current_status = snap;
        }
        if (renderer) {
            renderer->set_usb_status(static_cast<int>(snap.state),
                                     snap.device_name,
                                     snap.identifier);
        }
    }

    void run() {
        LOG_INFO << "USB supervisor: polling for iOS & Android USB devices every "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        kPollInterval).count() << " ms";
        while (running.load()) {
            auto usb_devs = enumerate_mobile_usb_devices();
            auto adb_devs = query_adb_usb_devices();

            std::set<std::string> current_keys;
            UsbStatusSnapshot next_status{};

            // 1. Check Apple iOS USB devices
            for (const auto& d : usb_devs) {
                const auto key = lower(d.serial);
                current_keys.insert(key);
                if (d.is_apple) {
                    const bool paired = pair_record_exists(d.serial);
                    if (known_keys.insert(key).second) {
                        LOG_NOTICE << "USB iOS device found: " << d.friendly_name
                                   << " (udid=" << d.serial
                                   << ", paired=" << (paired ? "yes" : "no - waiting for Trust") << ")";
                    }
                    next_status.device_name  = d.friendly_name;
                    next_status.identifier   = d.serial;
                    next_status.tunnel_ready = paired && probe_usbmuxd_ready();
                    next_status.state        = paired ? UsbState::IosTrustedReady
                                                      : UsbState::IosWaitTrust;
                }
            }

            // 2. Check Android USB devices (from ADB daemon or SetupAPI fallback)
            for (const auto& ad : adb_devs) {
                const auto key = "adb:" + lower(ad.serial);
                current_keys.insert(key);
                const bool authorized = (ad.state == "device");
                if (known_keys.insert(key).second) {
                    LOG_NOTICE << "USB Android device found via ADB: " << ad.model
                               << " (serial=" << ad.serial << ", state=" << ad.state << ")";
                }
                if (next_status.state == UsbState::NoDevice) {
                    next_status.device_name  = ad.model;
                    next_status.identifier   = ad.serial;
                    next_status.tunnel_ready = authorized;
                    next_status.state        = authorized ? UsbState::AndroidReady
                                                          : UsbState::AndroidWaitDebugging;
                }
                if (authorized && android_server && !android_server->is_streaming()) {
                    if (launched_android_serials.insert(ad.serial).second) {
                        LOG_NOTICE << "USB supervisor: auto-starting Android USB H.264 stream for "
                                   << ad.serial;
                        android_server->connect_usb_adb_async(ad.serial, ad.model);
                    }
                }
            }

            // SetupAPI fallback when Android phone is plugged in via USB before ADB sees it
            if (next_status.state == UsbState::NoDevice) {
                for (const auto& d : usb_devs) {
                    if (!d.is_apple) {
                        next_status.device_name  = d.friendly_name;
                        next_status.identifier   = d.serial;
                        next_status.tunnel_ready = false;
                        next_status.state        = UsbState::AndroidWaitDebugging;
                        break;
                    }
                }
            }

            // Clean up removed USB keys
            for (auto it = known_keys.begin(); it != known_keys.end(); ) {
                if (current_keys.find(*it) == current_keys.end()) {
                    LOG_INFO << "USB device disconnected: " << *it;
                    it = known_keys.erase(it);
                } else {
                    ++it;
                }
            }
            if (adb_devs.empty()) {
                launched_android_serials.clear();
            }

            update_status(next_status);

            constexpr auto slice = std::chrono::milliseconds(200);
            std::chrono::milliseconds remaining = kPollInterval;
            while (remaining.count() > 0 && running.load()) {
                auto step = remaining < slice ? remaining : slice;
                std::this_thread::sleep_for(step);
                remaining -= step;
            }
        }
        LOG_INFO << "USB supervisor: stopped";
    }
};

#else  // non-Windows stub

struct UsbSupervisor::Impl {
    std::atomic<bool>                 running{false};
    std::thread                       thread;
    mutable std::mutex                status_mtx;
    UsbStatusSnapshot                 current_status;
    ap::video::VideoRenderer*         renderer = nullptr;
    ap::android::AndroidMirrorServer* android_server = nullptr;
    void run() {}
};

#endif

UsbSupervisor::UsbSupervisor() : impl_(std::make_unique<Impl>()) {}
UsbSupervisor::~UsbSupervisor() { stop(); }

void UsbSupervisor::attach_renderer(ap::video::VideoRenderer* renderer) {
    impl_->renderer = renderer;
}

void UsbSupervisor::attach_android_server(ap::android::AndroidMirrorServer* android_server) {
    impl_->android_server = android_server;
}

UsbStatusSnapshot UsbSupervisor::status() const {
    std::lock_guard<std::mutex> lock(impl_->status_mtx);
    return impl_->current_status;
}

bool UsbSupervisor::start() {
#if defined(_WIN32)
    if (impl_->running.exchange(true)) return true;
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
#else
    LOG_INFO << "USB supervisor: not implemented on this platform";
    return false;
#endif
}

void UsbSupervisor::stop() {
#if defined(_WIN32)
    if (!impl_->running.exchange(false)) return;
    if (impl_->thread.joinable()) impl_->thread.join();
#endif
}

} // namespace ap::usb
