#pragma once

#include <cstdint>

namespace ap::airplay {

// Decides when a connected mirror link should be reset.
//
// The iPhone sends something at least once a second while it is mirroring, even
// when the picture does not change (heartbeat / stream report). A link that
// stays completely silent for several seconds is dead: the picture freezes and
// the old session keeps the receiver busy, so the iPhone cannot reconnect.
// When that happens main() disconnects the session and restarts the Bonjour
// advertisement so the receiver is immediately ready for a new connection.
//
// Pure logic, no clocks or threads: easy to test and cheap to call every tick.
class LinkWatchdog {
public:
    // Returns true exactly once for each connected link that has been silent
    // for longer than `timeout_s` seconds. `timeout_s <= 0` switches it off.
    // The link is re-armed as soon as it is reported inactive again.
    bool poll(bool link_active, int64_t last_rx_ns, int64_t now_ns, int timeout_s) {
        if (!link_active) {
            fired_ = false;
            return false;
        }
        if (timeout_s <= 0) {
            fired_ = false;
            return false;
        }
        if (fired_ || now_ns <= last_rx_ns) return false;
        const uint64_t elapsed_ns = static_cast<uint64_t>(now_ns) -
                                    static_cast<uint64_t>(last_rx_ns);
        const uint64_t timeout_ns = static_cast<uint64_t>(timeout_s) * 1'000'000'000ULL;
        if (elapsed_ns > timeout_ns) {
            fired_ = true;
            return true;
        }
        return false;
    }

private:
    bool fired_ = false;
};

} // namespace ap::airplay
