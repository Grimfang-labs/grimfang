#include "timeman.hpp"

#include <algorithm>

// ===========================================================================
// timeman.cpp - basic time management.
// ===========================================================================

namespace {
// Safety overhead subtracted from limits so we never flag on output/scheduling.
constexpr std::int64_t MOVE_OVERHEAD = 10;   // ms
// Hard floor on any timed budget. Under load a computed budget can come out
// zero or negative (increment-only edges, movetime smaller than overhead,
// clock jitter); flooring here guarantees no caller ever sees a <= 0 budget,
// so the engine always gets a moment to complete at least one iteration and
// return a legal move instead of flagging or reading garbage.
constexpr std::int64_t MIN_THINK_MS  = 5;    // ms
} // namespace

TimeBudget compute_budget(const Search::Limits& limits, Color us) {
    TimeBudget b;

    if (limits.infinite)
        return b;                       // useTime stays false

    if (limits.movetime > 0) {
        b.useTime = true;
        const std::int64_t mt     = limits.movetime;
        const std::int64_t margin = std::min<std::int64_t>(MOVE_OVERHEAD, mt / 2);
        b.hard = mt - margin;
        b.soft = b.hard;
    } else {
        const std::int64_t t   = limits.time[us];
        const std::int64_t inc = limits.inc[us];
        if (t > 0 || inc > 0) {
            b.useTime = true;
            const int mtg = (limits.movestogo > 0) ? limits.movestogo : 30;

            const std::int64_t soft = t / mtg + (inc * 3) / 4;
            const std::int64_t hard = std::min<std::int64_t>(t / 2, soft * 4);

            b.soft = soft - MOVE_OVERHEAD;
            b.hard = hard - MOVE_OVERHEAD;
        }
        // else: only depth / nodes given -> useTime stays false.
    }

    // Final floor: no timed budget is ever <= 0, and soft never exceeds hard.
    if (b.useTime) {
        b.hard = std::max<std::int64_t>(MIN_THINK_MS, b.hard);
        b.soft = std::max<std::int64_t>(MIN_THINK_MS, b.soft);
        b.soft = std::min<std::int64_t>(b.soft, b.hard);
    }
    return b;
}
