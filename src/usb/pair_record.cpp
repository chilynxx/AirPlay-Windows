#include "usb/pair_record.h"
#include "log.h"
#include "net/socket.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <shlobj.h>
    #pragma comment(lib, "shell32.lib")
    #pragma comment(lib, "ole32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

namespace ap::usb {

namespace {

std::string strip_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) if (c != '-') out.push_back(c);
    return out;
}

std::vector<std::string> candidate_udids(const std::string& udid) {
    std::vector<std::string> out;
    out.push_back(udid);
    auto stripped = strip_dashes(udid);
    if (stripped != udid) {
        out.push_back(stripped);
    } else if (udid.size() == 24) {
        out.push_back(udid.substr(0, 8) + "-" + udid.substr(8));
    }
    return out;
}

std::vector<std::string> candidate_filenames(const std::string& udid) {
    std::vector<std::string> out;
    for (const auto& u : candidate_udids(udid)) {
        out.push_back(u + ".plist");
    }
    return out;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    auto size = ifs.tellg();
    if (size < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), size);
    }
    return ifs.good() || ifs.eof();
}

#if defined(HAVE_LIBPLIST)

std::string get_string(plist_t root, const char* key) {
    plist_t v = plist_dict_get_item(root, key);
    if (!v || plist_get_node_type(v) != PLIST_STRING) return {};
    char* raw = nullptr;
    plist_get_string_val(v, &raw);
    std::string out = raw ? raw : "";
    if (raw) free(raw);
    return out;
}

void get_data(plist_t root, const char* key, std::vector<unsigned char>& out) {
    plist_t v = plist_dict_get_item(root, key);
    if (!v || plist_get_node_type(v) != PLIST_DATA) return;
    char*    buf = nullptr;
    uint64_t len = 0;
    plist_get_data_val(v, &buf, &len);
    if (buf && len) {
        out.assign(reinterpret_cast<unsigned char*>(buf),
                   reinterpret_cast<unsigned char*>(buf) + len);
    }
    if (buf) free(buf);
}

bool parse_pair_record_blob(const unsigned char* data, std::size_t size, PairRecord& out) {
    if (!data || size == 0) return false;
    plist_t root = nullptr;
    plist_from_memory(reinterpret_cast<const char*>(data),
                      static_cast<uint32_t>(size), &root, nullptr);
    if (!root) return false;
    out = {};
    out.host_id     = get_string(root, "HostID");
    out.system_buid = get_string(root, "SystemBUID");
    out.wifi_mac    = get_string(root, "WiFiMACAddress");
    get_data(root, "HostCertificate",   out.host_certificate);
    get_data(root, "HostPrivateKey",    out.host_private_key);
    get_data(root, "RootCertificate",   out.root_certificate);
    get_data(root, "RootPrivateKey",    out.root_private_key);
    get_data(root, "DeviceCertificate", out.device_certificate);
    plist_free(root);
    return !out.host_id.empty();
}

#endif // HAVE_LIBPLIST

void write_u32_le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t read_u32_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

} // namespace

std::string lockdown_directory() {
#if defined(_WIN32)
    PWSTR programdata_w = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0,
                                    nullptr, &programdata_w))) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, programdata_w, -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string base;
    if (len > 1) {
        base.assign(static_cast<std::size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, programdata_w, -1,
                            base.data(), len, nullptr, nullptr);
    }
    CoTaskMemFree(programdata_w);
    if (base.empty()) return {};
    return base + "\\Apple\\Lockdown";
#else
    return "/var/db/lockdown";
#endif
}

bool usbmuxd_read_pair_record(const std::string& udid, PairRecord& out) {
#if defined(HAVE_LIBPLIST)
    if (udid.empty()) return false;
    for (const auto& candidate : candidate_udids(udid)) {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCK) return false;
#if defined(_WIN32)
        DWORD tmo = 500;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(27015);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ap::net::close_socket(s);
            return false;
        }

        std::ostringstream xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            << "<plist version=\"1.0\"><dict>"
            << "<key>MessageType</key><string>ReadPairRecord</string>"
            << "<key>PairRecordID</key><string>" << candidate << "</string>"
            << "<key>ProgName</key><string>WinAirPlay</string>"
            << "<key>ClientVersionString</key><string>WinAirPlay-0.5</string>"
            << "</dict></plist>";
        const std::string body = xml.str();
        std::vector<uint8_t> pkt(16 + body.size());
        write_u32_le(pkt.data() + 0, static_cast<uint32_t>(pkt.size()));
        write_u32_le(pkt.data() + 4, 1); // version = 1 (plist protocol)
        write_u32_le(pkt.data() + 8, 8); // type = 8 (plist payload)
        write_u32_le(pkt.data() + 12, 1); // tag = 1
        std::memcpy(pkt.data() + 16, body.data(), body.size());

        if (ap::net::send_all(s, pkt.data(), static_cast<int>(pkt.size())) < 0) {
            ap::net::close_socket(s);
            continue;
        }

        uint8_t hdr[16] = {};
        int got = 0;
        while (got < 16) {
            int n = ::recv(s, reinterpret_cast<char*>(hdr + got), 16 - got, 0);
            if (n <= 0) break;
            got += n;
        }
        if (got < 16) {
            ap::net::close_socket(s);
            continue;
        }
        const uint32_t total_len = read_u32_le(hdr);
        if (total_len <= 16 || total_len > 256 * 1024) {
            ap::net::close_socket(s);
            continue;
        }
        const uint32_t payload_len = total_len - 16;
        std::vector<unsigned char> resp(payload_len);
        got = 0;
        while (got < static_cast<int>(payload_len)) {
            int n = ::recv(s, reinterpret_cast<char*>(resp.data() + got),
                           static_cast<int>(payload_len) - got, 0);
            if (n <= 0) break;
            got += n;
        }
        ap::net::close_socket(s);
        if (got < static_cast<int>(payload_len)) continue;

        plist_t root = nullptr;
        plist_from_memory(reinterpret_cast<const char*>(resp.data()),
                          static_cast<uint32_t>(resp.size()), &root, nullptr);
        if (!root) continue;
        std::vector<unsigned char> pair_blob;
        get_data(root, "PairRecordData", pair_blob);
        plist_free(root);

        if (!pair_blob.empty() &&
            parse_pair_record_blob(pair_blob.data(), pair_blob.size(), out)) {
            return true;
        }
    }
    return false;
#else
    (void)udid;
    (void)out;
    return false;
#endif
}

bool pair_record_exists(const std::string& udid) {
    if (udid.empty()) return false;
    auto dir = lockdown_directory();
    if (!dir.empty()) {
        for (const auto& name : candidate_filenames(udid)) {
#if defined(_WIN32)
            std::string full = dir + "\\" + name;
#else
            std::string full = dir + "/" + name;
#endif
            std::ifstream ifs(full, std::ios::binary);
            if (ifs) return true;
        }
    }
    PairRecord rec;
    return usbmuxd_read_pair_record(udid, rec);
}

bool load_pair_record(const std::string& udid, PairRecord& out) {
    if (udid.empty()) return false;
    auto dir = lockdown_directory();
    std::vector<unsigned char> blob;
    bool found = false;
    if (!dir.empty()) {
        for (const auto& name : candidate_filenames(udid)) {
#if defined(_WIN32)
            std::string full = dir + "\\" + name;
#else
            std::string full = dir + "/" + name;
#endif
            if (read_file(full, blob)) { found = true; break; }
        }
    }
    if (!found) {
        return usbmuxd_read_pair_record(udid, out);
    }

#if defined(HAVE_LIBPLIST)
    return parse_pair_record_blob(blob.data(), blob.size(), out);
#else
    (void)out;
    return true;
#endif
}

} // namespace ap::usb
