#pragma once

namespace ap {

// The product name shown in the window, the title bar and, by default, in the
// AirPlay list on the iPhone.
inline constexpr const char* kAppName = "WWinAirPlay";

// Previous built-in receiver names. Only these untouched defaults migrate to
// WWinAirPlay; any custom receiver name chosen by the user is preserved.
inline constexpr const char* kLegacyReceiverName = "AirPlay-Windows";
inline constexpr const char* kPreviousReceiverName = "WinAirPlay";

} // namespace ap
