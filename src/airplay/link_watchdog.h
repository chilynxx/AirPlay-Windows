#pragma once

#include <cstdint>

namespace ap::airplay {

enum class LinkHealth : uint8_t {
    Healthy       = 0,
    MicroStutter  = 1,
    Probing       = 2,
    ResetRequired = 3
};

// Decides when a connected mirror link is experiencing a transient Wi-Fi
// micro-stutter (requiring non-intrusive probing + IDR keyframe recovery)
// versus a dead link that should be reset.
//
// Pure logic, no clocks or threads: deterministic, easy to test, and cheap
// to call on every supervisor tick.
class LinkWatchdog {
public:
    // Returns true exactly once for each connected link that has been silent
    // for longer than `timeout_s` seconds. `timeout_s <= 0` switches it off.
    // The link is re-armed as soon as it is reported inactive again.
    bool poll(bool link_active, int64_t last_rx_ns, int64_t now_ns, int timeout_s) {
        if (!link_active) {
            fired_ = false;
            last_probe_ns_ = 0;
            health_ = LinkHealth::Healthy;
            return false;
        }
        if (timeout_s <= 0) {
            fired_ = false;
            last_probe_ns_ = 0;
            health_ = LinkHealth::Healthy;
            return false;
        }
        if (fired_ || now_ns <= last_rx_ns) return false;
        const uint64_t elapsed_ns = static_cast<uint64_t>(now_ns) -
                                    static_cast<uint64_t>(last_rx_ns);
        const uint64_t timeout_ns = static_cast<uint64_t>(timeout_s) * 1'000'000'000ULL;
        if (elapsed_ns > timeout_ns) {
            fired_ = true;
            health_ = LinkHealth::ResetRequired;
            return true;
        }
        return false;
    }

    // Multi-stage self-healing evaluation:
    //   - < 1.25s silence: Healthy
    //   - 1.25s .. timeout_s: MicroStutter / Probing (sets `should_probe = true`
    //     once every 1.0s so the server sends RTSP keep-alive + IDR resync without
    //     dropping the window)
    //   - > timeout_s: ResetRequired
    LinkHealth evaluate(bool link_active, int64_t last_rx_ns, int64_t now_ns,
                        int timeout_s, bool& should_probe) {
        should_probe = false;
        if (!link_active || timeout_s <= 0 || now_ns <= last_rx_ns) {
            last_probe_ns_ = 0;
            health_ = LinkHealth::Healthy;
            return health_;
        }
        const uint64_t elapsed_ns = static_cast<uint64_t>(now_ns) -
                                    static_cast<uint64_t>(last_rx_ns);
        const uint64_t timeout_ns = static_cast<uint64_t>(timeout_s) * 1'000'000'000ULL;
        constexpr uint64_t kStutterThresholdNs = 1'250'000'000ULL; // 1.25 seconds
        constexpr uint64_t kProbeIntervalNs    = 1'000'000'000ULL; // 1.00 second

        if (elapsed_ns < kStutterThresholdNs) {
            last_probe_ns_ = 0;
            health_ = LinkHealth::Healthy;
            return health_;
        }
        if (elapsed_ns <= timeout_ns) {
            if (last_probe_ns_ == 0 ||
                static_cast<uint64_t>(now_ns - last_probe_ns_) >= kProbeIntervalNs) {
                last_probe_ns_ = now_ns;
                should_probe = true;
                health_ = LinkHealth::Probing;
            } else if (health_ == LinkHealth::Healthy) {
                health_ = LinkHealth::MicroStutter;
            }
            return health_;
        }
        health_ = LinkHealth::ResetRequired;
        return health_;
    }

    LinkHealth health() const noexcept { return health_; }

private:
    bool       fired_         = false;
    int64_t    last_probe_ns_ = 0;
    LinkHealth health_        = LinkHealth::Healthy;
};

} // namespace ap::airplay
