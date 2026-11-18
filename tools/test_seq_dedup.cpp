// Regression test for audio/seq_dedup.h.
//
// The previous duplicate filter in AudioReceiver dropped fresh packets after
// the 16-bit RTP sequence number wrapped (audio faded out after ~10-25 min).
// This test replays several full sequence laps with the traffic patterns that
// broke it and checks that no first arrival is ever reported as a duplicate
// while real duplicates are still caught.

#include "audio/seq_dedup.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Faithful copy of the old logic, kept only to show what it did wrong.
struct LegacyFilter {
    std::vector<bool> seen = std::vector<bool>(65536, false);
    uint64_t pkts = 0;
    bool is_duplicate(uint16_t seq) {
        ++pkts;
        if (seen[seq]) return true;
        seen[seq] = true;
        const uint16_t erase_from = static_cast<uint16_t>(seq - 8192);
        if ((pkts & 0x3ff) == 0)
            for (int k = 0; k < 1024; ++k)
                seen[static_cast<uint16_t>(erase_from + k)] = false;
        return false;
    }
};

struct Result { uint64_t false_drops = 0, missed_dups = 0, firsts = 0; };

// `copies` arrivals per sequence number, `loss` probability of losing a whole
// sequence number, `swap_p` probability of swapping two nearby packets.
template <class Filter>
Result run(Filter& f, int copies, double loss, double swap_p, int laps,
           uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const uint32_t total = static_cast<uint32_t>(laps) * 65536u;
    const uint16_t base = static_cast<uint16_t>(rng());
    std::vector<uint32_t> order;           // absolute index of each arrival
    order.reserve(static_cast<size_t>(total) * copies);
    for (uint32_t i = 0; i < total; ++i) {
        if (u(rng) < loss) continue;
        for (int c = 0; c < copies; ++c) order.push_back(i);
    }
    for (size_t i = 3; i < order.size(); ++i)
        if (u(rng) < swap_p) std::swap(order[i], order[i - 2]);

    Result r;
    std::vector<uint8_t> seen_abs(total, 0);
    for (uint32_t idx : order) {
        const uint16_t s = static_cast<uint16_t>(base + idx);
        const bool first = !seen_abs[idx];
        seen_abs[idx] = 1;
        const bool dup = f.is_duplicate(s);
        if (first) { ++r.firsts; if (dup) ++r.false_drops; }
        else if (!dup) ++r.missed_dups;
    }
    return r;
}

void report(const char* name, const Result& r) {
    std::printf("      %-34s firsts=%8llu false_drops=%8llu missed_dups=%llu\n",
                name, static_cast<unsigned long long>(r.firsts),
                static_cast<unsigned long long>(r.false_drops),
                static_cast<unsigned long long>(r.missed_dups));
}

} // namespace

int main() {
    using ap::audio::SeqDedupWindow;
    constexpr int kLaps = 4;

    struct Case { const char* name; int copies; double loss; double swap; };
    const Case cases[] = {
        {"1 copy, clean",             1, 0.00, 0.00},
        {"2 copies per packet",       2, 0.00, 0.00},
        {"3 copies, 1% loss",         3, 0.01, 0.00},
        {"2 copies, 2% loss, reorder",2, 0.02, 0.05},
        {"1 copy, 1% loss, reorder",  1, 0.01, 0.05},
    };

    for (const auto& c : cases) {
        SeqDedupWindow fresh;
        const Result r = run(fresh, c.copies, c.loss, c.swap, kLaps, 1234);
        report(c.name, r);
        // Fresh packets must never be dropped. Duplicates are caught except
        // for the unavoidable few that arrive more than a window late.
        check(r.false_drops == 0, c.name);

        LegacyFilter old;
        const Result o = run(old, c.copies, c.loss, c.swap, kLaps, 1234);
        std::printf("      old logic on same traffic:        false_drops=%8llu\n",
                    static_cast<unsigned long long>(o.false_drops));
    }

    {   // Real duplicates must still be caught.
        SeqDedupWindow f;
        const Result r = run(f, 2, 0.0, 0.0, 2, 7);
        check(r.missed_dups == 0, "every real duplicate is caught");
    }
    {   // Sender restarts its sequence space at a random value.
        SeqDedupWindow f;
        uint64_t bad = 0;
        std::mt19937 rng(99);
        uint16_t prev_highest = 0;
        bool first_epoch = true;
        for (int epoch = 0; epoch < 200; ++epoch) {
            const uint16_t start = static_cast<uint16_t>(rng());
            // A restart that lands on top of the previous window is
            // indistinguishable from duplicates; real restarts create a new
            // receiver. Test the realistic case: a clearly separate range.
            if (!first_epoch &&
                static_cast<uint16_t>(prev_highest - start) < 2048 + 300)
                continue;
            for (int i = 0; i < 300; ++i) {
                if (f.is_duplicate(static_cast<uint16_t>(start + i))) ++bad;
            }
            prev_highest = static_cast<uint16_t>(start + 299);
            first_epoch = false;
        }
        check(bad == 0, "restart at random sequence number is not dropped");
    }
    {   // Strict behaviour on a tiny hand-written trace.
        SeqDedupWindow f;
        check(!f.is_duplicate(65534), "first packet accepted");
        check(!f.is_duplicate(65535), "next accepted");
        check(!f.is_duplicate(0),     "wraps to 0 accepted");
        check( f.is_duplicate(0),     "second 0 is duplicate");
        check(!f.is_duplicate(2),     "gap accepted");
        check(!f.is_duplicate(1),     "late packet in the gap accepted once");
        check( f.is_duplicate(1),     "late packet duplicate rejected");
        check( f.is_duplicate(65535), "old duplicate across the wrap rejected");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
