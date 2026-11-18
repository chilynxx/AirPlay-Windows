#pragma once

// Small Windows-only tuning helpers for a real-time media receiver. All of
// them compile to nothing on other platforms.

#if defined(_WIN32)
    #include <winsock2.h>
    #include <windows.h>
#endif

namespace ap::platform {

// Network readers, the decoder and audio playout are latency sensitive but
// use almost no CPU. A slightly higher priority keeps them from being
// starved by a busy desktop (which shows up as frozen video / dropped audio).
inline void raise_current_thread_priority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

// Windows 10 1709+ / Windows 11 may move a process into "EcoQoS" (low-power
// scheduling, coarse timers) when its window is covered or minimised or when
// battery saver is on. For a live mirror that means network threads wake up
// late and bursts of packets overflow socket buffers. Opt this process out.
// The API is resolved at run time so older Windows versions and older SDKs
// are unaffected.
inline void disable_process_power_throttling() {
#if defined(_WIN32)
    struct ThrottlingState {
        ULONG Version;        // PROCESS_POWER_THROTTLING_CURRENT_VERSION
        ULONG ControlMask;
        ULONG StateMask;
    };
    constexpr ULONG kVersion               = 1;
    constexpr ULONG kExecutionSpeed        = 0x1;  // ..._EXECUTION_SPEED
    constexpr ULONG kIgnoreTimerResolution = 0x4;  // ..._IGNORE_TIMER_RESOLUTION
    constexpr int   kProcessPowerThrottling = 4;   // ProcessPowerThrottling

    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto set_info = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation")));
    if (!set_info) return;

    ThrottlingState state{};
    state.Version     = kVersion;
    state.ControlMask = kExecutionSpeed | kIgnoreTimerResolution;
    state.StateMask   = 0;   // control bit set + state bit clear = never throttle
    set_info(GetCurrentProcess(), kProcessPowerThrottling, &state,
             static_cast<DWORD>(sizeof(state)));
#endif
}

// Keep the PC awake while a sender is connected (idle sleep would otherwise
// drop the connection). The state belongs to the calling thread, so always
// call this from the same long-lived thread (the render thread).
inline void set_keep_awake(bool on) {
#if defined(_WIN32)
    SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
                               : ES_CONTINUOUS);
#else
    (void)on;
#endif
}

} // namespace ap::platform
