#pragma once

namespace ap {

// Official product name shown in the window title bar, system tray, firewall rules,
// and by default in the AirPlay / Android receiver broadcast list.
inline constexpr const char* kAppName = "WinAirPlay";
inline constexpr const wchar_t* kAppNameW = L"WinAirPlay";

// Previous built-in receiver names. Only these untouched defaults migrate to
// WinAirPlay; any custom receiver name chosen by the user is preserved.
inline constexpr const char* kLegacyReceiverName = "AirPlay-Windows";
inline constexpr const char* kPreviousReceiverName = "WWinAirPlay";

} // namespace ap
