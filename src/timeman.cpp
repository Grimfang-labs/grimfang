#include "timeman.hpp"

#include <algorithm>

// ===========================================================================
// timeman.cpp - basic time management.
// ===========================================================================

namespace {
// Safety overhead subtracted from limits so we never flag on output/scheduling.
constexpr std::int64_t MOVE_OVERHEAD = 10;   // ms
} // namespace

TimeBudget compute_budget(const Search::Limits& limits, Color us) {
    TimeBudget b;

    if (limits.infinite)
        return b;                       // useTime stays false

    if (limits.movetime > 0) {
        b.useTime = true;
        const std::int64_t mt     = limits.movetime;
        const std::int64_t margin = std::min<std::int64_t>(MOVE_OVERHEAD, mt / 2);
        b.hard = std::max<std::int64_t>(1, mt - margin);
        b.soft = b.hard;
        return b;
    }

    const std::int64_t t   = limits.time[us];
    const std::int64_t inc = limits.inc[us];
    if (t > 0 || inc > 0) {
        b.useTime = true;
        const int mtg = (limits.movestogo > 0) ? limits.movestogo : 30;

        std::int64_t soft = t / mtg + (inc * 3) / 4;
        std::int64_t hard = std::min<std::int64_t>(t / 2, soft * 4);

        soft = std::max<std::int64_t>(1, soft - MOVE_OVERHEAD);
        hard = std::max<std::int64_t>(soft, hard - MOVE_OVERHEAD);
        hard = std::max<std::int64_t>(1, hard);

        b.soft = soft;
        b.hard = hard;
        return b;
    }

    // Only depth / nodes given: no time limit.
    return b;
}
