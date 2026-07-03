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

// Iterative-deepening driver. Emits a UCI `info` line per completed iteration
// and prints `bestmove <move>` exactly once when it returns. `stop` aborts the
// search asynchronously; the best move from the last completed iteration is
// kept.
Result search(Position& pos, const Limits& limits, std::atomic<bool>& stop);

// Fixed-depth search with no time management and no info output (used by tests
// and as the building block for iterative deepening). `stop` aborts cleanly.
Result search_fixed(Position& pos, int depth, std::atomic<bool>& stop);

// Convenience overload for callers that never abort (tests).
Result search_fixed(Position& pos, int depth);

// Node-capped iterative deepening (no info output). Uses the existing
// limits.nodes stop path; inert when nodeLimit == 0. Does not affect bench/UCI
// unless explicitly requested.
Result search_nodes(Position& pos, std::uint64_t nodeLimit, std::atomic<bool>& stop);
Result search_nodes(Position& pos, std::uint64_t nodeLimit);

// Default depth for `bench` (chosen to run in a few seconds single-threaded).
// Signature at this depth: 14717091 nodes.
constexpr int BENCH_DEFAULT_DEPTH = 6;

// Search a fixed list of positions to a fixed depth with time management
// disabled, printing an OpenBench-compatible summary. The total node count is
// deterministic and serves as the search regression signature. Returns the
// total nodes searched.
std::uint64_t bench(int depth = BENCH_DEFAULT_DEPTH);

} // namespace Search
