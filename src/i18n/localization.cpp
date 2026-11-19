#include "i18n/localization.h"

#include <array>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace ap::i18n {
namespace {

struct BilingualEntry {
    const char* en;
    const char* vi;
};

constexpr std::size_t kStringCount = static_cast<std::size_t>(Str::Count);

// Complete UTF-8 bilingual dictionary (English & Natural Vietnamese).
const std::array<BilingualEntry, kStringCount> kStrings = {{
    // AppName
    {"WinAirPlay", "WinAirPlay"},
    // YourSpace
    {"WORKSPACE", "KHÔNG GIAN"},
    // NavHome
    {"Home", "Trang chủ"},
    // NavMusic
    {"Audio Studio", "Âm thanh"},
    // NavMirror
    {"Screen Mirror", "Phản chiếu"},
    // NavUsb
    {"USB Direct", "Kết nối USB"},
    // NavAndroid
    {"Android Cast", "Android"},
    // NavSettings
    {"Settings", "Cài đặt"},

    // StatusDeviceConnected
    {"Device connected", "Đã kết nối thiết bị"},
    // StatusReceiverAvailable
    {"Receiver active", "Bộ thu sẵn sàng"},
    // StatusReadyToReceive
    {"Ready to receive", "Sẵn sàng kết nối"},
    // StatusConnected
    {"Connected", "Đã kết nối"},
    // StatusCheckFirewall
    {"Check firewall", "Kiểm tra tường lửa"},
    // StatusConnectedAudio
    {"Connected · Lossless Audio", "Đã kết nối · Âm thanh"},
    // StatusConnectedMirror
    {"Connected · AirPlay Mirroring", "Đã kết nối · Phản chiếu AirPlay"},
    // StatusConnectedUsb
    {"Connected · High-Speed USB Tunnel", "Đã kết nối · Cáp USB tốc độ cao"},
    // StatusConnectedAndroid
    {"Connected · Android Screen Mirror", "Đã kết nối · Phản chiếu Android"},
    // StatusWaitingForAirPlay
    {"Waiting for connection...", "Đang chờ kết nối..."},
    // StatusAutoReconnecting
    {"Auto-reconnecting...", "Đang tự động kết nối lại..."},
    // StatusAutoReconnectingDetail
    {"Stabilizing stream & requesting IDR keyframe...", "Đang khôi phục tín hiệu & đồng bộ khung hình IDR..."},
    // StatusAutoRecoveredNotice
    {"Stream link automatically recovered", "Đã tự động khôi phục luồng kết nối"},

    // BtnDisconnect
    {"Disconnect", "Ngắt kết nối"},
    // BtnReconnect
    {"Resync", "Đồng bộ lại"},
    // BtnRecord
    {"Record", "Ghi hình"},
    // BtnStopRecording
    {"Stop Rec", "Dừng ghi"},
    // BtnScreenshot
    {"Capture", "Chụp ảnh"},
    // BtnFullscreen
    {"Fullscreen", "Toàn màn hình"},
    // BtnHideInterface
    {"Minimal", "Ẩn giao diện"},
    // BtnRotation
    {"Rotate", "Xoay"},
    // BtnMute
    {"Mute", "Tắt tiếng"},
    // BtnUnmute
    {"Unmute", "Bật tiếng"},
    // BtnThemeDark
    {"Obsidian Dark", "Giao diện Tối"},
    // BtnThemeLight
    {"Alabaster Light", "Giao diện Sáng"},
    // BtnLangSwitchTooltip
    {"Switch language (English / Tiếng Việt)", "Chuyển đổi ngôn ngữ (Tiếng Việt / English)"},

    // DockModeWifi
    {"Wi-Fi", "Wi-Fi"},
    // DockModeUsb
    {"USB", "Cáp USB"},
    // DockPlatformIos
    {"iOS", "iOS"},
    // DockPlatformAndroid
    {"Android", "Android"},
    // DockTooltipMode
    {"Toggle Connection Mode: Wireless Wi-Fi vs. Wired USB", "Chuyển chế độ kết nối: Không dây Wi-Fi hoặc Cáp USB"},
    // DockTooltipPlatform
    {"Switch Target Platform: iOS AirPlay vs. Android Mirroring", "Chọn nền tảng: iOS AirPlay hoặc Phản chiếu Android"},
    // DockTooltipReconnect
    {"Manual Reconnect / Flush Decoder & Request IDR Keyframe", "Kết nối lại / Làm mới bộ giải mã & yêu cầu khung hình IDR"},
    // DockTooltipRotate
    {"Cycle Screen Rotation & Dynamic Aspect-Ratio Lock (Portrait / Landscape)", "Xoay khung hình & Tự động khớp tỷ lệ Dọc / Ngang"},
    // DockTooltipRecord
    {"Start / Stop Hardware MP4 Screen Recording (Shortcut: R)", "Bắt đầu / Dừng quay video màn hình MP4 (Phím tắt: R)"},
    // DockTooltipScreenshot
    {"Take Instant Lossless Screenshot (Shortcut: S)", "Chụp ảnh màn hình tức thì (Phím tắt: S)"},
    // DockTooltipFullscreen
    {"Toggle Fullscreen Stage (Shortcut: F / F11)", "Bật / Tắt chế độ Toàn màn hình (Phím tắt: F / F11)"},
    // DockTooltipMute
    {"Toggle Master Audio Mute", "Tắt / Bật âm thanh trên máy tính"},
    // DockTooltipVolume
    {"1:1 Synchronized Master Volume", "Âm lượng tổng đồng bộ 1:1 với Windows"},

    // PageTitleHome
    {"WinAirPlay Studio", "WinAirPlay Studio"},
    // PageTitleMusic
    {"Studio Audio Receiver", "Bộ thu Âm thanh Không dây"},
    // PageTitleMirror
    {"Live Screen Stage", "Trình chiếu Màn hình Trực tiếp"},
    // HeroHeading1
    {"Your screen. Pure clarity.", "Màn hình của bạn. Sắc nét tuyệt đối."},
    // HeroHeading2
    {"Zero-compromise wireless & USB mirroring.", "Phản chiếu Không dây & Cáp USB độ trễ siêu thấp."},
    // HeroSubtitleIosWifi
    {"Stream video, games, and lossless audio from iPhone or iPad over local Wi-Fi.", "Truyền tải hình ảnh, trò chơi và âm thanh từ iPhone hoặc iPad qua mạng Wi-Fi."},
    // HeroSubtitleIosUsb
    {"Plug in your iPhone or iPad via USB cable for <25ms ultra-low latency mirroring.", "Kết nối iPhone hoặc iPad bằng cáp USB với độ trễ siêu thấp dưới 25ms."},
    // HeroSubtitleAndroidWifi
    {"Mirror your Android phone wirelessly via ADB TCP/IP or direct H.264 stream on port 7001.", "Phản chiếu điện thoại Android không dây qua giao thức ADB TCP/IP hoặc luồng H.264 cổng 7001."},
    // HeroSubtitleAndroidUsb
    {"Plug in your Android device via USB with USB Debugging enabled for instant 60fps mirroring.", "Cắm cáp USB điện thoại Android (bật Gỡ lỗi USB) để phản chiếu 60fps tức thì."},

    // CardSelectOnDevice
    {"SELECT RECEIVER ON YOUR DEVICE", "CHỌN THIẾT BỊ THU TRÊN ĐIỆN THOẠI"},
    // CardReceiverSettingsTooltip
    {"Customize receiver name & preferences", "Tùy chỉnh tên bộ thu & cài đặt"},
    // CardAndroidQuickConnect
    {"ANDROID WIRELESS QUICK CONNECT (IP:PORT)", "KẾT NỐI NHANH ANDROID KHÔNG DÂY (IP:CỔNG)"},
    // CardAndroidConnectBtn
    {"Connect", "Kết nối"},
    // CardAndroidIpPlaceholder
    {"e.g. 192.168.1.50:5555", "VD: 192.168.1.50:5555"},

    // Step1IosWifiTitle
    {"Same Local Network", "Cùng mạng Wi-Fi"},
    // Step1IosWifiDesc
    {"Connect your iPhone or iPad and this PC to the same Wi-Fi or LAN network.", "Kết nối iPhone hoặc iPad và máy tính này vào cùng một mạng Wi-Fi / LAN."},
    // Step2IosWifiTitle
    {"Open Control Center", "Mở Trung tâm điều khiển"},
    // Step2IosWifiDesc
    {"Swipe down from the top-right corner of your iOS device and tap Screen Mirroring.", "Vuốt xuống từ góc trên bên phải màn hình iOS và chọn Phản chiếu màn hình."},
    // Step3IosWifiTitle
    {"Select WinAirPlay", "Chọn WinAirPlay"},
    // Step3IosWifiDesc
    {"Tap this receiver name in the list. Hardware-accelerated mirroring starts instantly.", "Chạm vào tên máy tính này trong danh sách. Hình ảnh sẽ hiển thị ngay lập tức."},

    // Step1IosUsbTitle
    {"Connect USB Cable", "Cắm cáp kết nối USB"},
    // Step1IosUsbDesc
    {"Plug your iPhone or iPad into this PC using a data-capable Lightning or USB-C cable.", "Kết nối iPhone hoặc iPad với máy tính bằng cáp truyền dữ liệu Lightning hoặc USB-C."},
    // Step2IosUsbTitle
    {"Trust This Computer", "Tin cậy máy tính này"},
    // Step2IosUsbDesc
    {"Unlock your iPhone, tap 'Trust' on the prompt, and enter your device passcode.", "Mở khóa iPhone, chạm vào 'Tin cậy' (Trust) trên thông báo và nhập mật mã thiết bị."},
    // Step3IosUsbTitle
    {"Mirror over USB Tunnel", "Phản chiếu qua cổng USB"},
    // Step3IosUsbDesc
    {"WinAirPlay automatically bridges the USB tunnel or USB Hotspot for <25ms latency.", "WinAirPlay tự động thiết lập đường truyền USB tốc độ cao với độ trễ dưới 25ms."},

    // Step1AndroidWifiTitle
    {"Enable Wireless Debugging", "Bật Gỡ lỗi không dây"},
    // Step1AndroidWifiDesc
    {"Open Developer Options on Android and enable Wireless Debugging (or TCP port 5555).", "Mở Tùy chọn nhà phát triển trên Android và bật Gỡ lỗi không dây (hoặc cổng TCP 5555)."},
    // Step2AndroidWifiTitle
    {"Enter Device IP Address", "Nhập địa chỉ IP thiết bị"},
    // Step2AndroidWifiDesc
    {"Enter your Android phone's IP above and click Connect, or stream directly to TCP :7001.", "Nhập địa chỉ IP điện thoại Android vào ô trên và nhấn Kết nối (hoặc truyền tới cổng 7001)."},
    // Step3AndroidWifiTitle
    {"Instant 60fps Stream", "Trình chiếu 60fps tức thì"},
    // Step3AndroidWifiDesc
    {"Auto-rotation and D3D11 hardware H.264/HEVC decoding activate automatically.", "Tự động xoay khung hình theo trò chơi và giải mã phần cứng D3D11 mượt mà."},

    // Step1AndroidUsbTitle
    {"Connect Android via USB", "Cắm cáp USB Android"},
    // Step1AndroidUsbDesc
    {"Connect your Android phone to this PC with a high-speed USB data cable.", "Kết nối điện thoại Android với máy tính bằng cáp dữ liệu USB tốc độ cao."},
    // Step2AndroidUsbTitle
    {"Allow USB Debugging", "Cho phép Gỡ lỗi USB"},
    // Step2AndroidUsbDesc
    {"Enable USB Debugging in Developer Options and tap 'Allow' on your phone screen.", "Bật Gỡ lỗi USB trong Tùy chọn nhà phát triển và nhấn 'Cho phép' trên màn hình điện thoại."},
    // Step3AndroidUsbTitle
    {"Automatic Direct Stream", "Tự động truyền hình ảnh"},
    // Step3AndroidUsbDesc
    {"WinAirPlay detects your Android device automatically and launches the H.264 tunnel.", "WinAirPlay tự động nhận diện thiết bị Android và khởi chạy luồng hình ảnh H.264."},

    // UsbBannerIosUntrustedTitle
    {"iPhone/iPad Detected via USB — Waiting for 'Trust This Computer'", "Đã phát hiện iPhone/iPad qua USB — Đang chờ 'Tin cậy máy tính này'"},
    // UsbBannerIosUntrustedDesc
    {"Please unlock your iOS device, tap 'Trust' on the alert dialog, and confirm with your passcode.", "Vui lòng mở khóa thiết bị iOS, nhấn 'Tin cậy' (Trust) trên hộp thoại và xác nhận mật mã."},
    // UsbBannerIosTrustedTitle
    {"iOS USB Device Trusted & Paired — High-Speed USB Tunnel Ready", "Thiết bị iOS đã Tin cậy & Ghép đôi — Đường truyền USB sẵn sàng"},
    // UsbBannerIosTrustedDesc
    {"Open Control Center on your iPhone/iPad and select WinAirPlay (or enable USB Personal Hotspot).", "Mở Trung tâm điều khiển trên iPhone/iPad và chọn WinAirPlay để phản chiếu qua cáp USB."},
    // UsbBannerAndroidUnauthTitle
    {"Android Device Detected via USB — Waiting for 'Allow USB Debugging'", "Đã phát hiện Android qua USB — Đang chờ 'Cho phép Gỡ lỗi USB'"},
    // UsbBannerAndroidUnauthDesc
    {"Check 'Always allow from this computer' on your Android screen and tap 'Allow'.", "Tích chọn 'Luôn cho phép từ máy tính này' trên màn hình điện thoại Android và nhấn 'Cho phép'."},
    // UsbBannerAndroidReadyTitle
    {"Android USB Device Authorized — Establishing Direct H.264 Stream", "Thiết bị Android đã cấp quyền — Đang thiết lập luồng H.264 trực tiếp"},
    // UsbBannerAndroidReadyDesc
    {"USB Debugging verified. Streaming low-latency video directly over the USB bus.", "Đã xác thực Gỡ lỗi USB. Đang truyền tải hình ảnh độ trễ thấp trực tiếp qua cáp USB."},
    // UsbBannerScanningTitle
    {"USB Plug-and-Play Supervisor Active — Waiting for USB Device", "Trình giám sát USB đang hoạt động — Đang chờ cắm cáp thiết bị"},
    // UsbBannerScanningDesc
    {"Connect any iPhone, iPad, or Android device via USB cable to start wired mirroring.", "Hãy cắm cáp USB từ iPhone, iPad hoặc điện thoại Android vào máy tính để bắt đầu."},

    // HelpLinkNotShowing
    {"Device not finding WinAirPlay?", "Không tìm thấy WinAirPlay trên điện thoại?"},
    // HelpPopupTitle
    {"Connection Troubleshooting", "Hướng dẫn khắc phục kết nối"},
    // HelpCloseTooltip
    {"Close troubleshooting guide", "Đóng hướng dẫn"},
    // HelpStep1Title
    {"1. Verify Wi-Fi Subnet or USB Cable", "1. Kiểm tra mạng Wi-Fi hoặc cáp USB"},
    // HelpStep1Desc
    {"Ensure both devices are on the same router subnet (no Guest Wi-Fi isolation) or use a data USB cable.", "Đảm bảo cả hai thiết bị kết nối cùng một bộ phát Wi-Fi hoặc sử dụng cáp USB truyền dữ liệu."},
    // HelpStep2Title
    {"2. Check VPN & Proxy Interfaces", "2. Kiểm tra VPN & Proxy"},
    // HelpStep2Desc
    {"Active VPNs can block mDNS/Bonjour multicast discovery. Enable LAN bypass in your VPN settings.", "VPN có thể chặn quảng bá mDNS/Bonjour. Hãy tắt VPN hoặc bật chế độ cho phép mạng LAN."},
    // HelpStep3Title
    {"3. Allow Windows Defender Firewall", "3. Cho phép Tường lửa Windows"},
    // HelpStep3Desc
    {"If a firewall banner appears at the top, click 'Allow on local network' to open TCP/UDP ports.", "Nếu có thông báo tường lửa, hãy nhấn 'Cho phép mạng nội bộ' để mở cổng kết nối TCP/UDP."},
    // HelpFooterNote
    {"Toggle Wi-Fi off and back on from your phone's Control Center to refresh discovery.", "Tắt và bật lại Wi-Fi trên điện thoại để làm mới danh sách thiết bị thu."},
    // FooterLocalNetwork
    {"WinAirPlay Engine · Hardware D3D11VA · AirPlay 2 & Android", "WinAirPlay Engine · Tăng tốc phần cứng D3D11VA · AirPlay 2 & Android"},

    // FirewallBannerAllowTitle
    {"Allow WinAirPlay through Windows Firewall", "Cho phép WinAirPlay qua Tường lửa Windows"},
    // FirewallBannerBlockedTitle
    {"Windows Firewall is blocking incoming connections", "Tường lửa Windows đang chặn kết nối đến"},
    // FirewallBannerUnavailableTitle
    {"Windows Firewall status check unavailable", "Không thể kiểm tra trạng thái Tường lửa Windows"},
    // FirewallBannerAllowDetail
    {"Grant local network access so iPhones, iPads, and Android devices can discover and stream to WinAirPlay.", "Cấp quyền truy cập mạng nội bộ để iPhone, iPad và Android có thể tìm thấy và truyền hình ảnh tới WinAirPlay."},
    // FirewallBtnWaiting
    {"Waiting for Windows...", "Đang chờ xác nhận..."},
    // FirewallBtnAllow
    {"Allow on local network", "Cho phép mạng nội bộ"},

    // MusicNowPlaying
    {"NOW PLAYING", "ĐANG PHÁT"},
    // MusicPaused
    {"PAUSED", "TẠM DỪNG"},
    // MusicProgressTooltip
    {"Playback position (synchronized with device)", "Vị trí phát nhạc (đồng bộ với thiết bị)"},
    // MusicPrevTooltip
    {"Previous track", "Bài trước"},
    // MusicPlayTooltip
    {"Play", "Phát"},
    // MusicPauseTooltip
    {"Pause", "Tạm dừng"},
    // MusicNextTooltip
    {"Next track", "Bài tiếp theo"},
    // MusicSendingCmd
    {"Sending command to device...", "Đang gửi lệnh tới thiết bị..."},
    // MusicRemoteUnavailable
    {"Remote media control unavailable", "Điều khiển từ xa hiện không khả dụng"},
    // MusicCmdFailed
    {"Command failed or unsupported by app", "Lệnh không thành công hoặc ứng dụng không hỗ trợ"},
    // MusicPlayingFooter
    {"Streaming bit-perfect audio synced with Windows WASAPI", "Đang phát âm thanh chất lượng cao đồng bộ chuẩn Windows WASAPI"},

    // DecodeGpuNote
    {"D3D11VA GPU acceleration active · Zero-copy pipeline", "Đã bật tăng tốc GPU D3D11VA · Giải mã phần cứng trực tiếp"},
    // DecodeCpuNote
    {"Software CPU decoding active · GPU available in Settings", "Đang giải mã bằng CPU · Có thể bật GPU trong Cài đặt"},
    // ScreenshotSavedToast
    {"Screenshot saved to folder", "Đã lưu ảnh chụp màn hình vào thư mục"},

    // SettingsTitle
    {"Preferences", "Cài đặt hệ thống"},
    // SettingsCloseTooltip
    {"Close preferences", "Đóng cài đặt"},
    // SettingsSectionLanguageTheme
    {"Appearance & Language", "Giao diện & Ngôn ngữ"},
    // SettingsLanguageLabel
    {"Interface Language", "Ngôn ngữ hiển thị"},
    // SettingsThemeLabel
    {"Color Theme", "Chủ đề màu sắc"},
    // SettingsThemeDarkOption
    {"Obsidian Dark (Studio Default)", "Đen Obsidian (Mặc định Studio)"},
    // SettingsThemeLightOption
    {"Crisp Alabaster (Light Mode)", "Trắng Alabaster (Giao diện Sáng)"},
    // SettingsDeviceFrameLabel
    {"Draw subtle device bezel around mirrored screen", "Hiển thị viền khung thiết bị quanh màn hình phản chiếu"},
    // SettingsSectionReceiverName
    {"Receiver Broadcast Name", "Tên hiển thị bộ thu"},
    // SettingsApplyName
    {"Apply Name", "Áp dụng tên"},
    // SettingsNameInvalidHint
    {"Use a valid name without dots or backslashes (max 50 UTF-8 bytes).", "Vui lòng dùng tên hợp lệ không chứa dấu chấm hoặc gạch chéo (tối đa 50 byte UTF-8)."},
    // SettingsNameSavedHint
    {"Broadcast via Bonjour/mDNS & Android discovery. Saved automatically.", "Hiển thị trong danh sách AirPlay & Android. Tự động lưu lại."},
    // SettingsSectionResolution
    {"Mirror Resolution Ceiling", "Độ phân giải phản chiếu tối đa"},
    // SettingsCustomRes
    {"Custom...", "Tùy chỉnh..."},
    // SettingsWidth
    {"Width", "Rộng"},
    // SettingsHeight
    {"Height", "Cao"},
    // SettingsResRangeHint
    {"Accepted range: 320-7680 x 240-4320", "Giới hạn cho phép: 320-7680 x 240-4320"},
    // SettingsResMaxHint
    {"Maximum bounding box advertised to device. Aspect ratio is always preserved.", "Độ phân giải tối đa quảng bá tới thiết bị. Tỷ lệ khung hình luôn được giữ nguyên."},
    // SettingsResReceivedFmt
    {"Active stream resolution: %d x %d", "Độ phân giải luồng hiện tại: %d x %d"},
    // SettingsAppliesNextConn
    {"Applies on next device connection", "Áp dụng ở lần kết nối tiếp theo"},
    // SettingsAppliesLive
    {"Applies immediately (no reconnect needed)", "Áp dụng ngay lập tức (không cần kết nối lại)"},
    // SettingsSectionDecoder
    {"Hardware Video Decoder & VSync", "Bộ giải mã Video & Đồng bộ khung hình"},
    // SettingsUseGpu
    {"Use D3D11VA GPU Hardware Acceleration", "Sử dụng tăng tốc phần cứng GPU (D3D11VA)"},
    // SettingsGpuHint
    {"Decodes H.264/HEVC directly on GPU surfaces with zero CPU readback.", "Giải mã H.264/HEVC trực tiếp trên card đồ họa, giảm tối đa tải CPU."},
    // SettingsCpuHint
    {"Uses multi-sliced libavcodec software threads on your CPU.", "Sử dụng bộ giải mã phần mềm đa luồng trên vi xử lý CPU."},
    // SettingsVsync
    {"Synchronize presentation with monitor refresh (VSync)", "Đồng bộ khung hình với tần số quét màn hình (VSync)"},
    // SettingsVsyncOnHint
    {"Eliminates screen tearing for smooth video playback.", "Loại bỏ hiện tượng xé hình, giúp chuyển động mượt mà."},
    // SettingsVsyncOffHint
    {"Presents frames immediately (saves ~8-16ms latency).", "Hiển thị khung hình ngay lập tức (giảm ~8-16ms độ trễ)."},
    // SettingsSectionStability
    {"Smart Auto-Reconnect & Stream Recovery", "Tự động Kết nối lại & Phục hồi Luồng phát"},
    // SettingsAutoRecoverLabel
    {"Watchdog timeout (seconds)", "Thời gian chờ tự phục hồi (giây)"},
    // SettingsAutoRecoverHint
    {"Probes silent links after 1.5s and resets stuck sessions after the timeout (0 = off).", "Tự động thăm dò khi nghẽn mạng sau 1.5s và làm mới phiên khi quá hạn (0 = tắt)."},
    // SettingsSectionRecording
    {"Screen Recording & Screenshots", "Ghi hình & Chụp ảnh màn hình"},
    // SettingsFolder
    {"Output Folder", "Thư mục lưu"},
    // SettingsBrowse
    {"Browse...", "Chọn..."},
    // SettingsBrowseDialogTitle
    {"Choose the recording & screenshot folder", "Chọn thư mục lưu video quay màn hình & ảnh chụp"},
    // SettingsVideoCodec
    {"Video Codec", "Chuẩn nén Video"},
    // SettingsEncoder
    {"Hardware Encoder", "Bộ mã hóa"},
    // SettingsBitrateMbps
    {"Bitrate (Mbps)", "Tốc độ bit (Mbps)"},
    // SettingsBitrateHint
    {"Higher bitrate preserves sharper detail during fast gaming motion.", "Bitrate cao hơn giúp giữ chi tiết sắc nét khi chơi game chuyển động nhanh."},
    // SettingsRecNextHint
    {"Encoder changes apply to the next recording session.", "Thay đổi bộ mã hóa sẽ áp dụng cho lần ghi hình tiếp theo."},
    // SettingsRecMp4Hint
    {"Records hardware-accelerated MP4 with synchronized stereo AAC audio.", "Ghi video MP4 tăng tốc phần cứng kèm âm thanh nổi AAC đồng bộ."},
    // SettingsStartRecording
    {"Start Recording", "Bắt đầu ghi hình"},
    // SettingsSectionAdvanced
    {"Advanced & Diagnostics", "Nâng cao & Chẩn đoán"},
    // SettingsShowDiagnostics
    {"Show real-time telemetry status bar", "Hiển thị thanh thông số kỹ thuật thời gian thực"},
    // SettingsSectionRotation
    {"Orientation & Aspect-Ratio Lock", "Hướng xoay & Tỷ lệ khung hình"},
    // SettingsRotationAuto
    {"Automatic (Follow Device Orientation)", "Tự động (Theo chiều xoay điện thoại)"},
    // SettingsRotation0
    {"Manual Lock: 0° (Natural)", "Khóa thủ công: 0° (Mặc định)"},
    // SettingsRotation90
    {"Manual Lock: 90° Clockwise (Landscape)", "Khóa thủ công: 90° Thuận chiều kim đồng hồ"},
    // SettingsRotation180
    {"Manual Lock: 180° Inverted", "Khóa thủ công: 180° Đảo ngược"},
    // SettingsRotation270
    {"Manual Lock: 270° Clockwise", "Khóa thủ công: 270° Thuận chiều kim đồng hồ"},
    // SettingsRotationHint
    {"Auto dynamically resizes the window between Portrait and Landscape when your phone rotates.", "Chế độ Tự động sẽ tự khớp khung cửa sổ Dọc/Ngang ngay khi bạn xoay điện thoại."},
    // SettingsSectionFramerate
    {"Frame Rate Target", "Giới hạn Tốc độ Khung hình (FPS)"},
    // SettingsFpsCap
    {"Max FPS", "FPS tối đa"},
    // SettingsFpsHint
    {"60 fps recommended for gaming and smooth UI scrolling.", "Khuyên dùng 60 fps để chơi game và cuộn giao diện mượt nhất."},
    // SettingsSectionCompression
    {"Video Codec Negotiation", "Đàm phán Chuẩn nén Video"},
    // SettingsAllowHevc
    {"Enable HEVC (H.265) 10-bit / High Efficiency", "Cho phép giải mã HEVC (H.265) hiệu suất cao"},
    // SettingsHevcOnHint
    {"Delivers higher visual quality at 40% lower network bandwidth.", "Cho chất lượng hình ảnh sắc nét hơn với băng thông mạng giảm 40%."},
    // SettingsHevcOffHint
    {"Forces H.264 baseline/high profile for maximum legacy compatibility.", "Buộc dùng chuẩn H.264 truyền thống để tương thích tối đa."},
    // SettingsSectionNetwork
    {"Network & Transport Endpoints", "Cổng Mạng & Giao thức Truyền tải"},
    // SettingsSectionShortcuts
    {"Keyboard Shortcuts", "Phím tắt Bàn phím"},
    // SettingsShortcutFullscreen
    {"F / F11 / Double-Click   Toggle Fullscreen", "F / F11 / Nháy đúp chuột   Toàn màn hình"},
    // SettingsShortcutHideUi
    {"H                        Toggle Minimal Chromeless Mode", "H                          Ẩn / Hiện khung giao diện"},
    // SettingsShortcutRecord
    {"R                        Start / Stop MP4 Recording", "R                          Bắt đầu / Dừng quay video MP4"},
    // SettingsShortcutScreenshot
    {"S                        Capture Instant Screenshot", "S                          Chụp ảnh màn hình tức thì"},
    // SettingsShortcutEsc
    {"Esc                      Exit Fullscreen / Restore UI / Quit", "Esc                        Thoát Toàn màn hình / Hiện UI / Thoát"},
    // SettingsResetHint
    {"Restore all WinAirPlay preferences to factory defaults.", "Khôi phục toàn bộ cài đặt WinAirPlay về trạng thái mặc định ban đầu."},
    // SettingsResetBtn
    {"Reset All Preferences", "Khôi phục mặc định"},
    // SettingsResetDone
    {"Factory default settings restored.", "Đã khôi phục cài đặt mặc định thành công."},

    // DiagStateIdle
    {"Idle", "Chờ kết nối"},
    // DiagStateConnected
    {"Connected", "Đã kết nối"},
    // DiagStateStreaming
    {"Streaming", "Đang truyền phát"},
    // DiagStatePaused
    {"Paused", "Tạm dừng"},
    // DiagStateReconnecting
    {"Auto-Recovering", "Đang tự khôi phục"},

    // TrayShow
    {"Show WinAirPlay", "Hiển thị WinAirPlay"},
    // TrayHide
    {"Hide to System Tray", "Ẩn xuống khay hệ thống"},
    // TrayExit
    {"Exit WinAirPlay", "Thoát WinAirPlay"},
    // SingleInstanceError
    {"Unable to initialize the single application instance.", "Không thể khởi tạo phiên chạy duy nhất của ứng dụng."}
}};

} // namespace

LocalizationManager& LocalizationManager::instance() {
    static LocalizationManager inst;
    return inst;
}

const char* LocalizationManager::get(Str id) const noexcept {
    const auto idx = static_cast<std::size_t>(id);
    if (idx >= kStringCount) return "";
    const Language lang = language();
    const auto& entry = kStrings[idx];
    return (lang == Language::Vietnamese) ? entry.vi : entry.en;
}

std::wstring LocalizationManager::get_wide(Str id) const {
    return utf8_to_wide(get(id));
}

std::wstring LocalizationManager::utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
#if defined(_WIN32)
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                        static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
#else
    std::wstring out;
    out.reserve(utf8.size());
    for (unsigned char c : utf8) out.push_back(static_cast<wchar_t>(c));
    return out;
#endif
}

std::string LocalizationManager::wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
#if defined(_WIN32)
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(wide.size());
    for (wchar_t wc : wide) out.push_back(static_cast<char>(wc));
    return out;
#endif
}

} // namespace ap::i18n
