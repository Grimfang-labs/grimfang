#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "move.hpp"
#include "types.hpp"

// ===========================================================================
// history.hpp - butterfly history heuristic (Stage 3, rung 3.4).
//
// The soft, long-range complement to killers. Killers say "this exact quiet
// move refuted a sibling at this ply"; history says "quiet moves with this
// (side, from, to) have tended to cause cutoffs anywhere in this search". It is
// a per-search score table over quiet moves, rewarded on a quiet beta cutoff
// and penalised for the quiets that were tried first and failed ("gravity").
//
// Search-local: cleared at the start of each search, never persisted.
// ===========================================================================

// Gravity target: repeated bonuses converge on +/-MAX_HISTORY instead of
// overflowing, so it must sit well inside the int16_t range.
constexpr int MAX_HISTORY       = 16384;

// Clamp on the depth-scaled bonus (depth*depth), keeping single updates small.
constexpr int HISTORY_BONUS_CAP = 1600;

inline int history_bonus(int depth) {
    const int b = depth * depth;
    return b < HISTORY_BONUS_CAP ? b : HISTORY_BONUS_CAP;
}

struct HistoryTable {
    std::int16_t table[COLOR_NB][SQ_NB][SQ_NB];   // [side][from][to]

    void clear() { std::memset(table, 0, sizeof(table)); }

    // Gravity update: pull the entry toward zero in proportion to |delta| so it
    // converges rather than saturating the int16 rails.
    static void apply(std::int16_t& entry, int delta) {
        const int v = entry + delta - entry * std::abs(delta) / MAX_HISTORY;
        entry = static_cast<std::int16_t>(v);
    }

    void update(Color c, Move m, int delta) {
        apply(table[c][m.from_sq()][m.to_sq()], delta);
    }

    int get(Color c, Move m) const {
        return table[c][m.from_sq()][m.to_sq()];
    }
};
