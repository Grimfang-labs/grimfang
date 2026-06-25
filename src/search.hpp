#pragma once

#include <atomic>
#include <cstdint>

#include "move.hpp"
#include "score.hpp"
#include "types.hpp"

// ===========================================================================
// search.hpp - negamax alpha-beta + quiescence + iterative deepening.
//
// Stage 2 has no transposition table, killers, history or SEE: move ordering
// is captures-first by MVV-LVA, quiet moves in generation order, plus the
// previous iteration's best move tried first at the root.
// ===========================================================================

class Position;

namespace Search {

// Parsed `go` parameters. time/inc are indexed by Color (WHITE/BLACK).
struct Limits {
    int           time[COLOR_NB] = { 0, 0 };
    int           inc[COLOR_NB]  = { 0, 0 };
    int           movestogo      = 0;
    int           depth          = 0;
    int           movetime       = 0;
    std::uint64_t nodes          = 0;
    bool          infinite       = false;
};

struct Result {
    Move          bestMove = MOVE_NONE;
    Value         score    = 0;
    int           depth    = 0;
    std::uint64_t nodes    = 0;
};

// Fixed-depth search with no time management and no info output (used by tests
// and as the building block for iterative deepening). `stop` aborts cleanly.
Result search_fixed(Position& pos, int depth, std::atomic<bool>& stop);

// Convenience overload for callers that never abort (tests).
Result search_fixed(Position& pos, int depth);

} // namespace Search
