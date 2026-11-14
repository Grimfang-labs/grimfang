#pragma once

#include <atomic>
#include <cstdint>

#include "move.hpp"
#include "score.hpp"
#include "types.hpp"

// ===========================================================================
// search.hpp - public search interface: iterative-deepening negamax PVS with
// a transposition table, null-move / reverse-futility pruning, late move
// reductions, killer + history move ordering and aspiration windows. See the
// header block in search.cpp for the details of each technique.
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
    int aspirationDelta     = 25;    // initial +/- window half-width (cp)
    int aspirationWidenPct  = 200;   // widen factor on fail-high/low: delta = delta * pct / 100

    // Null-move pruning: R = nmpBase + depth / nmpDepthDiv
    //                      + min((staticEval - beta) / nmpEvalDiv, nmpEvalMax).
    int nmpBase             = 2;     // constant reduction term
    int nmpDepthDiv         = 3;     // depth divisor (>= 1)
    int nmpEvalDiv          = 200;   // + min((staticEval - beta) / nmpEvalDiv, nmpEvalMax)
    int nmpEvalMax          = 3;

    // Reverse futility pruning (static null move).
    int rfpMaxDepth         = 8;     // deepest ply RFP may fire
    int rfpMargin           = 100;   // centipawns of margin per ply of remaining depth
                                     // (depth - 1 plies when improving)

    // Internal iterative reduction: depth - 1 at depth >= iirMinDepth when
    // there is no TT move.
    int iirMinDepth         = 4;

    // Singular extensions: at depth >= seMinDepth, a TT move whose
    // lower-bound entry has depth >= depth - seTtDepthMargin is verified by
    // searching the other moves at depth (depth - 1) / 2 against
    // singularBeta = ttValue - seBetaMargin * depth / 8.
    int seMinDepth          = 7;
    int seTtDepthMargin     = 3;
    int seBetaMargin        = 16;    // cp per 8 plies of depth (2 cp/ply)

    // Late move pruning: at non-PV depth <= lmpMaxDepth, skip the remaining
    // quiets once (lmpBase + depth^2) / (2 - improving) moves were searched.
    int lmpMaxDepth         = 8;
    int lmpBase             = 3;

    // Futility pruning: at non-PV depth <= fpMaxDepth, skip the remaining
    // quiets when staticEval + fpBase + fpMargin * depth <= alpha.
    int fpMaxDepth          = 6;
    int fpBase              = 80;
    int fpMargin            = 90;

    // SEE pruning at non-PV depth <= seePruneMaxDepth: skip a quiet with
    // SEE < -seeQuietMargin * depth, a noisy move with
    // SEE < -seeNoisyMargin * depth^2.
    int seePruneMaxDepth    = 8;
    int seeQuietMargin      = 60;
    int seeNoisyMargin      = 25;

    // Late move reductions. The base reduction comes from a precomputed
    // table, in plies:  base/100 + ln(depth) * ln(moveNumber) / (div/100),
    // separately for quiet and noisy moves. Adjustments below are in
    // hundredths of a ply; the final reduction is rounded and clamped so the
    // reduced search keeps depth >= 1.
    int lmrMinDepth         = 3;     // node depth at/above which LMR may reduce
    int lmrMinMoveIndex     = 3;     // moves already searched before LMR may reduce
    int lmrQuietBasePct     = 80;    // quiet table: constant term (x100)
    int lmrQuietDivPct      = 240;   // quiet table: ln*ln divisor (x100)
    int lmrNoisyBasePct     = 0;     // noisy table: constant term (x100)
    int lmrNoisyDivPct      = 320;   // noisy table: ln*ln divisor (x100)
    int lmrPvPct            = 100;   // less at PV nodes
    int lmrCheckPct         = 100;   // less when the move gives check
    int lmrKillerPct        = 100;   // less for killers
    int lmrImprovingPct     = 100;   // more when not improving
    int lmrTtCapturePct     = 100;   // more when the TT move is a capture
    int lmrCutNodePct       = 100;   // more at expected cut nodes
    int lmrHistoryDiv       = 8000;  // quiet history / div plies less (more if < 0)

    // Quiescence delta pruning: skip a non-promotion capture when
    // standPat + victim value + margin <= alpha.
    int qsDeltaMargin       = 200;   // centipawns

    // History heuristic: bonus = min(mult * depth * depth / 100, cap).
    int historyBonusMultPct = 100;   // bonus multiplier, percent (100 == 1.0)
    int historyBonusCap     = 1600;  // clamp on the depth-scaled bonus
};

// Base LMR reduction before adjustments, in 1/1024 ply, for a move that is
// the moveNumber-th (1-based) searched at a node of the given depth. Exposed
// for tests; the search uses a table filled from this function.
int lmr_base_reduction(const Tunables& t, bool quiet, int depth, int moveNumber);

// The live tunable values, updated by UCI `setoption` (see uci.cpp).
const Tunables& active_tunables();
void            set_tunables(const Tunables& t);

// Reset all persistent search state (history tables) for a new game. Called
// on `ucinewgame`, by bench before each position and by datagen before each
// game. Does not touch the transposition table. Must not run concurrently with
// a search.
void clear();

// Iterative-deepening driver. Emits a UCI `info` line per completed iteration
// and prints `bestmove <move>` exactly once when it returns. `stop` aborts the
// search asynchronously; the best move from the last completed iteration is
// kept.
Result search(Position& pos, const Limits& limits, std::atomic<bool>& stop);

// Fixed-depth search with no time management and no info output (used by
// tests). `stop` aborts cleanly. Every search_fixed overload calls clear()
// first, so results are independent of earlier searches (except via the TT).
Result search_fixed(Position& pos, int depth, std::atomic<bool>& stop);

// Convenience overload for callers that never abort (tests).
Result search_fixed(Position& pos, int depth);

// Like search_fixed, but when useAspiration is false every depth uses a full
// (-INF,+INF) window — for aspiration-vs-full-window equivalence tests.
Result search_fixed(Position& pos, int depth, bool useAspiration);

// Node-capped iterative deepening (no info output). Uses the existing
// limits.nodes stop path; inert when nodeLimit == 0. Does not affect bench/UCI
// unless explicitly requested. Keeps the persistent history (like a game
// search); callers wanting a fresh start call clear() first (datagen does so
// at the start of each game).
Result search_nodes(Position& pos, std::uint64_t nodeLimit, std::atomic<bool>& stop);
Result search_nodes(Position& pos, std::uint64_t nodeLimit);

// Default depth for `bench` (chosen so the signature is a few million nodes
// and runs in a few seconds single-threaded). The current signature at this
// depth is recorded in tools/BENCH_SIG (it changes with every search change).
constexpr int BENCH_DEFAULT_DEPTH = 12;

// Search a fixed list of positions to a fixed depth with time management
// disabled, printing an OpenBench-compatible summary. The total node count is
// deterministic and serves as the search regression signature. Returns the
// total nodes searched.
std::uint64_t bench(int depth = BENCH_DEFAULT_DEPTH);

} // namespace Search
