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
    // When false, every ID depth uses a full (-INF,+INF) window (tests only).
    bool          aspiration     = true;
};

struct Result {
    Move          bestMove = MOVE_NONE;
    Value         score    = 0;
    int           depth    = 0;
    std::uint64_t nodes    = 0;
};

// ---------------------------------------------------------------------------
// Tunables - search constants exposed as UCI `spin` options for SPSA tuning.
//
// Every default is EXACTLY today's hardcoded value, so at defaults the search
// is byte-identical to the pre-tuning engine (bench signature unchanged).
//
// Naming convention for scaled options: a `...Pct` suffix means "divide the
// stored value by 100", so an option value of 150 represents the factor 1.5.
// (UCI `spin` options are integers and carry no free-text description, so the
// scale is encoded in the name; the exact formula is documented here and at
// the point of use.)
//
// The active values are snapshotted into the Searcher ONCE per search via
// active_tunables(); the hot path reads plain struct members, so there is no
// per-node lookup and NPS is unaffected.
// ---------------------------------------------------------------------------
struct Tunables {
    // Aspiration windows (ID driver).
    int aspirationDelta     = 120;   // initial +/- window half-width (cp)
    int aspirationWidenPct  = 150;   // widen factor on fail-high/low: delta = delta * pct / 100

    // Null-move pruning: R = nmpBase + depth / nmpDepthDiv.
    int nmpBase             = 3;     // constant reduction term
    int nmpDepthDiv         = 3;     // depth divisor (>= 1)

    // Reverse futility pruning (static null move).
    int rfpMaxDepth         = 8;     // deepest ply RFP may fire
    int rfpMargin           = 100;   // centipawns of margin per ply of remaining depth

    // Late move reductions.
    int lmrMinDepth         = 3;     // node depth at/above which LMR may reduce
    int lmrMinMoveIndex     = 3;     // 0-based move index from which LMR may reduce
    int lmrBaseReduction    = 3;     // starting R for an eligible late quiet move
    int lmrDepth6Extra      = 1;     // extra R once depth >= lmrDepth6At
    int lmrDepth6At         = 6;     // depth threshold for lmrDepth6Extra
    int lmrDeepExtra        = 1;     // extra R once move index >= lmrDeepAt
    int lmrDeepAt           = 12;    // move-index threshold for lmrDeepExtra
    int lmrPvReduction      = 1;     // R reduction applied at PV nodes
    int lmrKillerReduction  = 1;     // R reduction applied to killer moves

    // History heuristic: bonus = min(mult * depth * depth / 100, cap).
    int historyBonusMultPct = 100;   // bonus multiplier, percent (100 == 1.0)
    int historyBonusCap     = 1600;  // clamp on the depth-scaled bonus
};

// The live tunable values, updated by UCI `setoption` (see uci.cpp).
const Tunables& active_tunables();
void            set_tunables(const Tunables& t);

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

// Like search_fixed, but when useAspiration is false every depth uses a full
// (-INF,+INF) window — for aspiration-vs-full-window equivalence tests.
Result search_fixed(Position& pos, int depth, bool useAspiration);

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
