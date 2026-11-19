#pragma once

#include <string>
#include <vector>

namespace ap::usb {

// Subset of the Apple Mobile Device Service "pair record" stored at
//   %ProgramData%\Apple\Lockdown\<UDID>.plist
// or retrieved directly from usbmuxd (127.0.0.1:27015) once the user
// tapped "Trust This Computer" on the iPhone/iPad.
struct PairRecord {
    std::string                host_id;            // UUID, e.g. "ABCD1234-..."
    std::string                system_buid;        // host machine UUID
    std::string                wifi_mac;           // device WiFi MAC iOS sends back
    std::vector<unsigned char> host_certificate;   // PEM
    std::vector<unsigned char> host_private_key;   // PEM
    std::vector<unsigned char> root_certificate;   // PEM
    std::vector<unsigned char> root_private_key;   // PEM
    std::vector<unsigned char> device_certificate; // PEM
};

// Canonical Lockdown directory for the current host (e.g.
// "C:\ProgramData\Apple\Lockdown" on Windows, "/var/db/lockdown"
// on macOS/Linux). Empty string on failure.
std::string lockdown_directory();

// True if a pair record exists for this UDID (either on disk in the
// Lockdown directory or via the local usbmuxd daemon on 127.0.0.1:27015).
bool pair_record_exists(const std::string& udid);

// Read and parse the pair record from disk or usbmuxd. Returns false
// (and leaves `out` untouched) if the device is not yet trusted.
bool load_pair_record(const std::string& udid, PairRecord& out);

// Query the local Apple Mobile Device Service (usbmuxd on 127.0.0.1:27015)
// for a pair record by UDID.
bool usbmuxd_read_pair_record(const std::string& udid, PairRecord& out);

} // namespace ap::usb
