#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ap::android { class AndroidMirrorServer; }
namespace ap::video   { class VideoRenderer; }

namespace ap::usb {

enum class UsbState : uint8_t {
    NoDevice             = 0,
    IosWaitTrust         = 1,
    IosTrustedReady      = 2,
    AndroidWaitDebugging = 3,
    AndroidReady         = 4
};

struct UsbStatusSnapshot {
    UsbState    state = UsbState::NoDevice;
    std::string device_name;
    std::string identifier;   // iOS UDID or Android ADB serial
    bool        tunnel_ready = false;
};

// Complete Plug-and-Play USB Mirroring Supervisor for both iOS and Android:
//   - For iOS: enumerates Apple USB devices (SetupAPI VID_05AC + usbmuxd on
//     127.0.0.1:27015), verifies Lockdown / usbmuxd trust pair records,
//     establishes a local TCP tunnel to lockdownd / AirPlay over USB, and
//     prompts the user on the UI when waiting for "Trust This Computer".
//   - For Android: enumerates Android USB devices (SetupAPI OEM VIDs + ADB
//     daemon on 127.0.0.1:5037), detects "unauthorized" vs "device" states,
//     prompts the user on the UI when waiting for "Allow USB Debugging", and
//     automatically triggers high-bitrate H.264 USB mirroring via AndroidMirrorServer.
class UsbSupervisor {
public:
    UsbSupervisor();
    ~UsbSupervisor();

    UsbSupervisor(const UsbSupervisor&)            = delete;
    UsbSupervisor& operator=(const UsbSupervisor&) = delete;

    void attach_renderer(ap::video::VideoRenderer* renderer);
    void attach_android_server(ap::android::AndroidMirrorServer* android_server);

    // Spawn the polling & tunnel thread. Idempotent.
    bool start();

    // Signal the polling thread to exit and join. Idempotent.
    void stop();

    UsbStatusSnapshot status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::usb
