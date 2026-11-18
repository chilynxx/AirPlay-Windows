#pragma once

#include <array>
#include <cstdint>

namespace ap::audio {

// Sliding-window duplicate filter for 16-bit RTP sequence numbers.
//
// Replaces the old "65 536-entry bitset + erase 1024 bits every 1024th
// packet" logic in AudioReceiver. That version only cleaned up when the
// packet that happened to land on `pkts % 1024 == 0` was a fresh audio
// packet. With a sender that repeats each packet (or with any loss /
// keep-alive traffic) the clean-up silently stopped, stale "seen" bits
// survived the 16-bit wrap-around and fresh packets were thrown away as
// duplicates — audio faded out after roughly 10-25 minutes.
//
// Here the state is only the highest sequence number seen plus a bitmap of
// the last kWindow sequence numbers behind it. Moving forward clears exactly
// the slots that were skipped, so no stale bit can survive a wrap.
class SeqDedupWindow {
public:
    // Returns true when `seq` was already seen (drop it); otherwise records
    // it and returns false. A jump that cannot be explained by normal
    // reordering (stream restart, new RTP epoch) re-synchronises the window.
    bool is_duplicate(uint16_t seq) {
        if (!have_) {
            restart(seq);
            return false;
        }
        const uint16_t ahead = static_cast<uint16_t>(seq - highest_);
        if (ahead == 0) return true;
        if (ahead < 0x8000u) {
            if (ahead >= kWindow) {
                // Large forward gap (packet loss burst or stream restart).
                bits_.fill(0);
            } else {
                for (uint16_t i = 1; i <= ahead; ++i)
                    clear_bit(static_cast<uint16_t>(highest_ + i));
            }
            highest_ = seq;
            set_bit(seq);
            return false;
        }
        const uint16_t behind = static_cast<uint16_t>(highest_ - seq);
        if (behind < kWindow) {
            if (test_bit(seq)) return true;   // late copy of a packet we have
            set_bit(seq);                      // late but new (reordering)
            return false;
        }
        // Older than anything we remember: the sender restarted its sequence
        // space. Start over from this packet instead of guessing.
        restart(seq);
        return false;
    }

    void reset() { have_ = false; }

private:
    static constexpr uint16_t kWindow = 2048;   // must divide 65 536
    static_assert((65536u % kWindow) == 0, "window must divide the seq space");

    void restart(uint16_t seq) {
        bits_.fill(0);
        highest_ = seq;
        have_    = true;
        set_bit(seq);
    }
    void set_bit(uint16_t s)   { bits_[(s % kWindow) >> 6] |=  (1ULL << (s & 63)); }
    void clear_bit(uint16_t s) { bits_[(s % kWindow) >> 6] &= ~(1ULL << (s & 63)); }
    bool test_bit(uint16_t s) const {
        return (bits_[(s % kWindow) >> 6] >> (s & 63)) & 1ULL;
    }

    std::array<uint64_t, kWindow / 64> bits_{};
    uint16_t highest_ = 0;
    bool     have_    = false;
};

} // namespace ap::audio
