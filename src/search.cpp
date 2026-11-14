#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "eval.hpp"
#include "history.hpp"
#include "killers.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "see.hpp"
#include "sync.hpp"
#include "timeman.hpp"
#include "tt.hpp"

// ===========================================================================
// search.cpp - negamax alpha-beta + quiescence + iterative deepening.
//
// Move ordering: the transposition-table move first, then captures that do
// not lose material (static exchange evaluation >= 0) by MVV-LVA plus capture
// history, then the two killers, then losing captures (SEE < 0), then the
// remaining quiets by butterfly + 1/2-ply continuation history. A shared
// transposition table provides depth-preferred cutoffs at non-PV nodes and a
// best-move hint everywhere. Mate scores are made node-relative on store /
// re-rooted on probe. A per-ply search stack records the move played at each
// ply (and its continuation-history slice) with sentinels below the root.
// On a beta cutoff the cutoff move is rewarded and the moves of the same kind
// tried before it are penalised (history gravity); captures tried before
// any cutoff move are penalised in capture history. All history tables are
// kept across the searches of one game (ThreadData).
// Quiescence skips captures that lose material by SEE.
//
// Late move reductions: R from a log(depth) * log(moveNumber) table (quiet
// and noisy variants), adjusted for PV / cut nodes, checks, killers, the
// improving flag, a capture TT move and the quiet history score; a reduced
// probe that beats alpha is re-searched at full depth.
//
// PV-node classification: a node is a PV node iff it is reached by always
// following the first (best) move from the root. TT cutoffs are taken only at
// non-PV, non-root nodes so the principal variation is searched in full.
//
// Aspiration windows (ID driver only): from depth 4, each iteration opens a
// narrow window around the previous score and widens on fail-high / fail-low.
// Near-mate previous scores keep the full window.
// ===========================================================================

namespace {

// Static piece values used for MVV-LVA move ordering (shared with SEE).
constexpr const int (&orderValue)[PIECE_TYPE_NB] = SEE::VALUE;

constexpr int CAPTURE_BONUS     =  1'000'000;
constexpr int PROMOTION_BONUS   =    900'000;
constexpr int TT_MOVE_BONUS     = 10'000'000;   // root previous-best move
// Losing captures (SEE < 0) are ordered after every quiet move: this base plus
// any MVV-LVA / capture-history term stays far below the lowest quiet score
// (butterfly + two continuation-history terms, each > -MAX_HISTORY).
constexpr int BAD_CAPTURE_BONUS = 600'000;
static_assert(BAD_CAPTURE_BONUS - 16 * 900 - MAX_HISTORY > 3 * MAX_HISTORY,
              "bad captures must order above every quiet move");
static_assert(BAD_CAPTURE_BONUS + 16 * 900 + MAX_HISTORY < KILLER2_BONUS,
              "bad captures must order below the killers");
// Quiescence stops at the first move scored below this (a losing capture).
constexpr int QS_BAD_CAPTURE_LIMIT = BAD_CAPTURE_BONUS + 100'000;

// Capture history is added to the MVV-LVA term scaled down by this divisor.
constexpr int CAPTURE_HISTORY_DIV = 2;

// Search stack: one entry per ply plus STACK_OFFSET sentinels below the root
// so (ss - 1) .. (ss - 4) are always valid.
constexpr int STACK_OFFSET = 4;

struct StackEntry {
    Move            move       = MOVE_NONE;   // move played from this ply
    Piece           moved      = NO_PIECE;    // piece that made it
    Value           staticEval = VALUE_NONE;  // static eval at this ply (none in check)
    PieceToHistory* contHist   = nullptr;     // continuation slice of `move`
};

// Poll the stop conditions roughly every this many nodes.
constexpr std::uint64_t POLL_INTERVAL = 2048;

// ---------------------------------------------------------------------------
// Live tunables. Defaults are exactly the previously hardcoded constants, so
// the search is byte-identical until a UCI `setoption` changes a value. The
// Searcher snapshots these once per search (see Searcher::iterate); the
// per-node hot path reads only the snapshot's plain members.
//
// The RFP / LMR / NMP / aspiration / history constants that used to live here
// as file-scope `constexpr`s are now fields of Search::Tunables (search.hpp).
// ---------------------------------------------------------------------------
Search::Tunables g_tunables;

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Persistent per-thread search state. Everything a search learns that should
// carry over to the next search of the same game lives here: butterfly,
// continuation and capture history (later correction history). It
// survives between `go` commands and is reset by Search::clear() (ucinewgame,
// bench, datagen game start). Search-local scratch (killers, PV table) stays
// in Searcher.
//
// There is one search thread today, so one instance. With multiple threads
// each worker gets its own ThreadData (index = thread id) and Search::clear()
// resets them all.
// ---------------------------------------------------------------------------
struct ThreadData {
    HistoryTable        history;
    ContinuationHistory contHist;
    CaptureHistory      captureHistory;

    void clear() {
        history.clear();
        contHist.clear();
        captureHistory.clear();
    }
};

ThreadData g_mainThreadData;

std::string score_to_uci(Value score) {
    if (std::abs(score) >= VALUE_MATE_IN_MAX_PLY) {
        const int mate = (score > 0) ? (VALUE_MATE - score + 1) / 2
                                     : -((VALUE_MATE + score + 1) / 2);
        return "mate " + std::to_string(mate);
    }
    return "cp " + std::to_string(static_cast<int>(score));
}

class Searcher {
public:
    Searcher(std::atomic<bool>& stop, ThreadData& td)
        : stop_(stop), td_(td), history_(td.history) {}

    Search::Result iterate(Position& pos, const Search::Limits& limits, bool printInfo) {
        nodes_     = 0;
        seldepth_  = 0;
        aborted_   = false;
        startTime_ = Clock::now();

        // Snapshot the live tunables ONCE for this whole search. Every node
        // then reads plain struct members (t_.*) -- no global lookup, no
        // indirection, so NPS is unaffected by exposing these as options.
        t_ = Search::active_tunables();
        for (int q = 0; q < 2; ++q)
            for (int d = 0; d < LMR_TABLE_SIZE; ++d)
                for (int n = 0; n < LMR_TABLE_SIZE; ++n)
                    lmrTable_[q][d][n] = Search::lmr_base_reduction(t_, q == 1, d, n);

        // One search == one generation. Entries from earlier searches stay in
        // the table but age out under replacement. Killers are search-local
        // and start empty; history persists in ThreadData across searches
        // (reset by Search::clear()).
        TT.new_search();
        killers_.clear();
        for (StackEntry& e : stack_) {
            e = StackEntry{};
            e.contHist = &td_.contHist.sentinel();
        }

        // Time budget (no-op when only depth/nodes/infinite were given).
        const TimeBudget tb = compute_budget(limits, pos.side_to_move());
        useTime_    = tb.useTime;
        softMs_     = tb.soft;
        hardMs_     = tb.hard;
        limitNodes_ = limits.nodes;

        const int maxDepth = (limits.depth > 0)
                                 ? std::min(limits.depth, MAX_PLY - 1)
                                 : MAX_PLY - 1;

        Search::Result result;

        MoveList rootMoves;
        generate_legal(pos, rootMoves);
        if (rootMoves.count == 0) {
            result.bestMove = MOVE_NONE;
            return result;
        }
        result.bestMove = rootMoves.moves[0];   // legal fallback

        Value prevScore = 0;
        bool  havePrev  = false;

        for (int depth = 1; depth <= maxDepth; ++depth) {
            // Soft limit: don't start a new iteration if we are unlikely to
            // finish it within the budget.
            if (useTime_ && depth > 1 && elapsed_ms() >= softMs_)
                break;

            // Aspiration: from depth 4 onward, search a narrow window around the
            // previous iteration's score. Fail-high / fail-low widen (by the
            // factor t_.aspirationWidenPct/100, ~1.5x at default) from the
            // fail-soft score and re-search the same depth. Depths < 4 and
            // near-mate prev scores keep the full window. Widening is monotonic
            // and capped at ±VALUE_INFINITE.
            //
            // Initial δ (t_.aspirationDelta, default 120, not classic 20): against
            // this NNUE, shallow ID scores routinely swing outside ±20, and
            // gradual widening from 20 thrashed (bench +20% nodes). δ=120 still
            // tightens vs ±INF and drops nodes at bench depths 6/8/10 vs the
            // full-window baseline. Both δ and the widen factor are hand-picked
            // workaround values, never tuned -- hence exposed as options.
            Value alpha = -VALUE_INFINITE;
            Value beta  = VALUE_INFINITE;
            Value delta = t_.aspirationDelta;
            if (limits.aspiration
                && depth >= 4
                && havePrev
                && std::abs(prevScore) < VALUE_MATE_IN_MAX_PLY) {
                alpha = prevScore - delta;
                beta  = prevScore + delta;
            }

            Value score = 0;
            while (true) {
                score = negamax(pos, depth, alpha, beta, 0,
                                /*pvNode=*/true, /*cutNode=*/false, /*prevWasNull=*/false);
                if (aborted_)
                    break;

                if (score <= alpha) {
                    // Fail low: widen down around the returned score. Stay silent.
                    alpha = std::max<Value>(score - delta, -VALUE_INFINITE);
                    // Grow monotonically (>= +1) so the window always expands and
                    // the loop terminates even if the widen factor is set <= 100%.
                    delta = std::max<Value>(delta * t_.aspirationWidenPct / 100, delta + 1);
                } else if (score >= beta) {
                    // Fail high: widen up around the returned score. Stay silent.
                    beta  = std::min<Value>(score + delta, VALUE_INFINITE);
                    delta = std::max<Value>(delta * t_.aspirationWidenPct / 100, delta + 1);
                } else {
                    break;   // score inside window: exact for this depth
                }
            }

            const bool haveMove = pvLength_[0] > 0;
            if (aborted_) {
                // Discard this depth (including any interrupted fail-high/low
                // re-search). Keep the last fully completed iteration; depth 1
                // may still yield a usable move if it found one before abort.
                if (depth == 1 && haveMove) {
                    result.score    = score;
                    result.depth    = depth;
                    result.bestMove = pvTable_[0][0];
                    if (printInfo) print_info(depth, score, pos);
                }
                break;
            }

            result.score    = score;
            result.depth    = depth;
            if (haveMove) result.bestMove = pvTable_[0][0];
            prevScore = score;
            havePrev  = true;

            if (printInfo) print_info(depth, score, pos);

            // A proven mate cannot improve with more depth.
            if (std::abs(score) >= VALUE_MATE_IN_MAX_PLY)
                break;

            if (limitNodes_ && nodes_ >= limitNodes_)
                break;
        }

        result.nodes = nodes_;
        return result;
    }

    std::uint64_t nodes() const { return nodes_; }

private:
    std::atomic<bool>& stop_;
    std::uint64_t      nodes_      = 0;
    std::uint64_t      limitNodes_ = 0;
    int                seldepth_   = 0;
    bool               aborted_    = false;

    // Per-search snapshot of the live tunables (see iterate()).
    Search::Tunables   t_;

    Clock::time_point startTime_;
    bool              useTime_ = false;
    std::int64_t      softMs_  = 0;
    std::int64_t      hardMs_  = 0;

    Move pvTable_[MAX_PLY][MAX_PLY];
    int  pvLength_[MAX_PLY] = { 0 };

    StackEntry stack_[MAX_PLY + STACK_OFFSET + 1];

    // Base LMR reductions [quiet][depth][moveNumber] in 1/1024 ply, filled
    // from the tunables at the start of each search.
    static constexpr int LMR_TABLE_SIZE = 64;
    int lmrTable_[2][LMR_TABLE_SIZE][LMR_TABLE_SIZE];

    ThreadData&   td_;        // persistent state (history tables)
    KillerTable   killers_;   // search-local
    HistoryTable& history_;   // == td_.history

    std::int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - startTime_).count();
    }

    // Returns true once the search must abort. Polls time/nodes/stop flag only
    // every POLL_INTERVAL nodes to keep the hot path cheap.
    bool should_stop() {
        if (aborted_)
            return true;
        if ((nodes_ & (POLL_INTERVAL - 1)) != 0)
            return false;
        if (stop_.load(std::memory_order_relaxed)
            || (limitNodes_ && nodes_ >= limitNodes_)
            || (useTime_ && elapsed_ms() >= hardMs_))
            aborted_ = true;
        return aborted_;
    }

    // Captured piece type of a noisy move (CaptureHistory::NO_CAPTURE for a
    // non-capturing promotion).
    static int captured_index(const Position& pos, Move m) {
        if (m.type_of() == EN_PASSANT)
            return PAWN;
        const Piece victim = pos.piece_on(m.to_sq());
        return victim == NO_PIECE ? CaptureHistory::NO_CAPTURE : type_of(victim);
    }

    // Quiet-move history: butterfly plus the 1- and 2-ply continuation terms
    // (ss == nullptr in quiescence: butterfly only).
    int quiet_history(const Position& pos, Move m, const StackEntry* ss) const {
        int h = history_.get(pos.side_to_move(), m);
        if (ss) {
            const int pi = history_piece_index(pos.piece_on(m.from_sq()));
            const Square to = m.to_sq();
            h += (*(ss - 1)->contHist)[pi][to] + (*(ss - 2)->contHist)[pi][to];
        }
        return h;
    }

    void update_continuation(const StackEntry* ss, Piece pc, Square to, int delta) {
        const int pi = history_piece_index(pc);
        for (int k = 1; k <= 2; ++k)
            if ((ss - k)->move.is_ok())
                HistoryTable::apply((*(ss - k)->contHist)[pi][to], delta);
    }

    // Ordering score of a noisy move (capture, en passant or promotion):
    // MVV-LVA plus capture history, in the good band (above the killers)
    // unless it is a capture that loses material by SEE (bad band, between
    // the killers and the quiets). Quiet moves are ordered by killers then
    // quiet_history(); the TT move comes first (see MovePicker).
    int score_noisy(const Position& pos, Move m) const {
        const MoveType mt   = m.type_of();
        const Square   to   = m.to_sq();
        const bool     isEp = (mt == EN_PASSANT);
        const bool     isCapture = isEp || (pos.piece_on(to) != NO_PIECE);

        const Piece moved = pos.piece_on(m.from_sq());
        int score = td_.captureHistory.get(moved, to, captured_index(pos, m))
                  / CAPTURE_HISTORY_DIV;
        if (isCapture) {
            const PieceType victim   = isEp ? PAWN : type_of(pos.piece_on(to));
            const PieceType attacker = type_of(pos.piece_on(m.from_sq()));
            // SEE can only come out negative when the attacker is worth more
            // than the victim; otherwise skip the exchange evaluation.
            const bool good = orderValue[victim] >= orderValue[attacker]
                           || see_ge(pos, m, 0);
            score += (good ? CAPTURE_BONUS : BAD_CAPTURE_BONUS)
                   + orderValue[victim] * 16 - orderValue[attacker];
        }
        if (mt == PROMOTION)
            score += PROMOTION_BONUS + orderValue[m.promotion_type()];

        return score;
    }

    // ---- Staged move picker ----------------------------------------------
    // Yields the moves of a node one at a time in exactly the order a stable
    // descending sort by score_move() would give, but lazily:
    //   1. the TT move, validated with Position::is_legal, before any
    //      generation (a node that cuts on it never generates moves);
    //   2. noisy moves scoring above the killers (good captures, promotions),
    //   3. killer 1, killer 2 (only when they are legal quiets here),
    //   4. losing captures,
    //   5. the remaining quiets, scored by history only when reached.
    // Each band is picked by selection (first maximum, shifting the rest so
    // ties keep generation order) instead of being fully sorted.
    // Quiescence (qsearch, not in check) yields the TT move only when it is
    // noisy and stops after the good noisy moves; in check it yields every
    // evasion with quiets ordered by butterfly history alone (ss == nullptr).
    class MovePicker {
    public:
        MovePicker(const Searcher& s, const Position& pos, Move ttMove, Move k1, Move k2,
                   const StackEntry* ss, bool qsNoisyOnly)
            : s_(s), pos_(pos), tt_(ttMove), k1_(k1), k2_(k2), ss_(ss),
              qsNoisyOnly_(qsNoisyOnly) {
            if (!(tt_.is_ok() && pos_.is_legal(tt_)
                  && (!qsNoisyOnly_ || !is_quiet_move(pos_, tt_))))
                tt_ = MOVE_NONE;
        }

        // Stop yielding quiet moves (killers included) from now on.
        void skip_quiets() { skipQuiets_ = true; }

        // Number of moves yielded so far (0 at the end: no legal move).
        int yielded() const { return yielded_; }

        // Next move, or MOVE_NONE when exhausted. `score` receives the
        // move's ordering score (as score_move() would give it).
        Move next(int& score) {
            while (true) {
                switch (stage_) {
                case Stage::TT:
                    stage_ = Stage::GENERATE;
                    if (tt_.is_ok()) {
                        score = TT_MOVE_BONUS;
                        ++yielded_;
                        return tt_;
                    }
                    break;

                case Stage::GENERATE:
                    generate();
                    stage_ = Stage::GOOD_NOISY;
                    break;

                case Stage::GOOD_NOISY:
                    if (goodCur_ < goodEnd_) {
                        ++yielded_;
                        return pick(goodCur_++, goodEnd_, score);
                    }
                    stage_ = qsNoisyOnly_ ? Stage::DONE : Stage::KILLER1;
                    break;

                case Stage::KILLER1:
                    stage_ = Stage::KILLER2;
                    if (haveK1_ && !skipQuiets_) {
                        score = KILLER1_BONUS;
                        ++yielded_;
                        return k1_;
                    }
                    break;

                case Stage::KILLER2:
                    stage_ = Stage::BAD_NOISY;
                    if (haveK2_ && !skipQuiets_) {
                        score = KILLER2_BONUS;
                        ++yielded_;
                        return k2_;
                    }
                    break;

                case Stage::BAD_NOISY:
                    if (badCur_ < badEnd_) {
                        ++yielded_;
                        return pick(badCur_++, badEnd_, score);
                    }
                    stage_ = Stage::SCORE_QUIETS;
                    break;

                case Stage::SCORE_QUIETS:
                    if (skipQuiets_) {
                        stage_ = Stage::DONE;
                        break;
                    }
                    for (int i = quietCur_; i < quietEnd_; ++i)
                        scores_[i] = s_.quiet_history(pos_, moves_[i], ss_);
                    stage_ = Stage::QUIETS;
                    break;

                case Stage::QUIETS:
                    if (quietCur_ < quietEnd_ && !skipQuiets_) {
                        ++yielded_;
                        return pick(quietCur_++, quietEnd_, score);
                    }
                    stage_ = Stage::DONE;
                    break;

                case Stage::DONE:
                    return MOVE_NONE;
                }
            }
        }

    private:
        enum class Stage { TT, GENERATE, GOOD_NOISY, KILLER1, KILLER2, BAD_NOISY,
                           SCORE_QUIETS, QUIETS, DONE };

        // Generate, drop the TT move, and split into three bands kept in
        // generation order: good noisy [0, goodEnd_), bad noisy
        // [badCur_, badEnd_) and quiets [quietCur_, quietEnd_), all in one
        // buffer (good from the front, quiets from the back; the bad noisy
        // moves go to a side buffer then follow the good ones).
        void generate() {
            MoveList list;
            if (qsNoisyOnly_)
                generate_captures(pos_, list);
            else
                generate_legal(pos_, list);

            Move bad[MoveList::CAPACITY];
            int  badScores[MoveList::CAPACITY];
            int  nGood = 0, nBad = 0, nQuiet = 0;
            Move quiets[MoveList::CAPACITY];

            for (int i = 0; i < list.count; ++i) {
                const Move m = list.moves[i];
                if (m == tt_)
                    continue;
                if (is_quiet_move(pos_, m)) {
                    if (m == k1_ && k1_.is_ok())      haveK1_ = true;
                    else if (m == k2_ && k2_.is_ok()) haveK2_ = true;
                    else                              quiets[nQuiet++] = m;
                    continue;
                }
                const int sc = s_.score_noisy(pos_, m);
                if (sc > KILLER1_BONUS) {
                    moves_[nGood]  = m;
                    scores_[nGood] = sc;
                    ++nGood;
                } else {
                    bad[nBad]       = m;
                    badScores[nBad] = sc;
                    ++nBad;
                }
            }

            goodCur_ = 0;
            goodEnd_ = nGood;
            badCur_  = nGood;
            for (int i = 0; i < nBad; ++i) {
                moves_[nGood + i]  = bad[i];
                scores_[nGood + i] = badScores[i];
            }
            badEnd_   = nGood + nBad;
            quietCur_ = badEnd_;
            for (int i = 0; i < nQuiet; ++i)
                moves_[quietCur_ + i] = quiets[i];
            quietEnd_ = quietCur_ + nQuiet;
        }

        // Move the first highest-scoring move of [cur, end) to `cur`,
        // shifting the moves before it up by one (stable), and return it.
        Move pick(int cur, int end, int& score) {
            int best = cur;
            for (int i = cur + 1; i < end; ++i)
                if (scores_[i] > scores_[best])
                    best = i;
            const Move m  = moves_[best];
            const int  sc = scores_[best];
            for (int i = best; i > cur; --i) {
                moves_[i]  = moves_[i - 1];
                scores_[i] = scores_[i - 1];
            }
            moves_[cur]  = m;
            scores_[cur] = sc;
            score = sc;
            return m;
        }

        const Searcher&   s_;
        const Position&   pos_;
        Move              tt_;
        const Move        k1_, k2_;
        const StackEntry* ss_;
        const bool        qsNoisyOnly_;

        Stage stage_      = Stage::TT;
        bool  skipQuiets_ = false;
        bool  haveK1_     = false;
        bool  haveK2_     = false;
        int   yielded_    = 0;
        int   goodCur_ = 0, goodEnd_ = 0;
        int   badCur_ = 0, badEnd_ = 0;
        int   quietCur_ = 0, quietEnd_ = 0;

        Move moves_[MoveList::CAPACITY];
        int  scores_[MoveList::CAPACITY];
    };

    void print_info(int depth, Value score, const Position& /*pos*/) const {
        const std::int64_t ms  = elapsed_ms();
        const std::uint64_t nps =
            (ms > 0) ? (nodes_ * 1000ULL) / static_cast<std::uint64_t>(ms) : nodes_ * 1000ULL;

        // Assemble the whole line, then emit it under the shared IO lock so it
        // can't interleave with a concurrent `readyok` from the UCI thread.
        std::ostringstream oss;
        oss << "info depth " << depth
            << " seldepth " << seldepth_
            << " score " << score_to_uci(score)
            << " nodes " << nodes_
            << " nps " << nps
            << " hashfull " << TT.hashfull()
            << " time " << ms
            << " pv";
        for (int i = 0; i < pvLength_[0]; ++i)
            oss << ' ' << to_uci(pvTable_[0][i]);
        oss << '\n';
        sync_cout(oss.str());
    }

    // ---- Quiescence (fail-soft) -----------------------------------------
    // Captures/promotions only, from a stand-pat bound; every legal evasion
    // when in check (no stand-pat: bestScore starts at -VALUE_MATE + ply so a
    // checkmated node with zero evasions returns a mate score).
    //
    // The TT is probed (cutoffs at non-PV nodes with any stored depth, the TT
    // move ordered first) and written at exit with depth 0. The cached static
    // eval is reused on a hit, and a bound-compatible TT score tightens the
    // stand-pat. Losing captures (SEE < 0) and captures that cannot lift the
    // stand-pat to alpha even with a margin (delta pruning) are skipped.
    Value quiescence(Position& pos, Value alpha, Value beta, int ply, bool pvNode) {
        ++nodes_;
        if (ply > seldepth_) seldepth_ = ply;
        if (should_stop())   return alpha;
        if (ply >= MAX_PLY - 1)
            return Eval::evaluate(pos);

        const Key  key     = pos.key();
        const bool inCheck = pos.checkers() != 0;

        // ---- Transposition-table probe ---------------------------------
        // Copy what we need out of the entry now: deeper recursion can
        // overwrite the slot the pointer refers to.
        TTEntry*   tte     = nullptr;
        const bool ttHit   = TT.probe(key, tte);
        const Move  ttMove  = ttHit ? tte->move() : MOVE_NONE;
        const Value ttValue = ttHit ? value_from_tt(tte->value(), ply) : VALUE_NONE;
        const Value ttEval  = ttHit ? tte->eval() : VALUE_NONE;
        const Bound ttBound = ttHit ? tte->bound() : BOUND_NONE;

        if (ttHit && !pvNode
            && (ttBound == BOUND_EXACT
                || (ttBound == BOUND_LOWER && ttValue >= beta)
                || (ttBound == BOUND_UPPER && ttValue <= alpha)))
            return ttValue;

        const Value alphaOrig = alpha;
        Value bestScore;
        Value staticEval = VALUE_NONE;
        Value standPat   = VALUE_NONE;

        if (inCheck) {
            bestScore = static_cast<Value>(-VALUE_MATE + ply);
        } else {
            staticEval = (ttEval != VALUE_NONE) ? ttEval : Eval::evaluate(pos);
            standPat   = staticEval;

            // A TT score is a better estimate than the static eval when its
            // bound points the right way (never trust a mate score here).
            if (ttHit
                && std::abs(ttValue) < VALUE_MATE_IN_MAX_PLY
                && (ttBound & (ttValue > standPat ? BOUND_LOWER : BOUND_UPPER)))
                standPat = ttValue;

            bestScore = standPat;
            if (bestScore >= beta) {
                if (!ttHit)
                    TT.store(key, MOVE_NONE, bestScore, staticEval, 0, BOUND_LOWER, ply);
                return bestScore;
            }
            if (bestScore > alpha)
                alpha = bestScore;
        }

        // Not in check: the TT move (if noisy) and the captures /
        // promotions that do not lose material by SEE; losing captures are
        // never searched. In check: every evasion.
        MovePicker mp(*this, pos, ttMove, MOVE_NONE, MOVE_NONE, nullptr,
                      /*qsNoisyOnly=*/!inCheck);

        Move bestMove = MOVE_NONE;
        int  moveScore = 0;

        for (Move m; (m = mp.next(moveScore)).is_ok(); ) {
            if (!inCheck) {
                // Delta pruning: even winning the victim outright plus a
                // margin cannot reach alpha. Promotions are never pruned.
                if (m.type_of() != PROMOTION) {
                    const PieceType victim = (m.type_of() == EN_PASSANT)
                                                 ? PAWN
                                                 : type_of(pos.piece_on(m.to_sq()));
                    const Value futility = standPat + orderValue[victim] + t_.qsDeltaMargin;
                    if (futility <= alpha) {
                        bestScore = std::max(bestScore, futility);
                        continue;
                    }
                }
            }

            pos.make_move(m);
            const Value score = -quiescence(pos, -beta, -alpha, ply + 1, pvNode);
            pos.unmake_move(m);

            if (aborted_)
                return alpha;

            if (score > bestScore) {
                bestScore = score;
                if (score > alpha) {
                    bestMove = m;
                    if (score >= beta)
                        break;
                    alpha = score;
                }
            }
        }

        const Bound bound = (bestScore >= beta)     ? BOUND_LOWER
                          : (bestScore > alphaOrig) ? BOUND_EXACT
                                                    : BOUND_UPPER;
        TT.store(key, bestMove, bestScore, staticEval, 0, bound, ply);

        return bestScore;
    }

    // ---- History updates on a beta cutoff ------------------------------
    // A quiet cutoff move becomes a killer and gets a butterfly +
    // continuation bonus; the quiets searched before it get the matching
    // malus. A noisy cutoff move gets a capture-history bonus. Noisy moves
    // searched before the cutoff move are penalised either way.
    void update_histories(const Position& pos, const StackEntry* ss, int ply, int depth,
                          Move best, bool bestIsQuiet,
                          const Move* quiets, int quietCount,
                          const Move* noisy, int noisyCount) {
        const Color stm   = pos.side_to_move();
        const int   bonus = history_bonus(depth, t_.historyBonusMultPct, t_.historyBonusCap);

        if (bestIsQuiet) {
            killers_.update(ply, best);
            history_.update(stm, best, bonus);
            update_continuation(ss, pos.piece_on(best.from_sq()), best.to_sq(), bonus);
            for (int j = 0; j < quietCount; ++j) {
                history_.update(stm, quiets[j], -bonus);
                update_continuation(ss, pos.piece_on(quiets[j].from_sq()),
                                    quiets[j].to_sq(), -bonus);
            }
        } else {
            td_.captureHistory.update(pos.piece_on(best.from_sq()), best.to_sq(),
                                      captured_index(pos, best), bonus);
        }

        for (int j = 0; j < noisyCount; ++j)
            td_.captureHistory.update(pos.piece_on(noisy[j].from_sq()), noisy[j].to_sq(),
                                      captured_index(pos, noisy[j]), -bonus);
    }

    // ---- Negamax alpha-beta (fail-soft) --------------------------------
    // cutNode: a non-PV node expected to fail high (the first move of an
    // expected cut node's parent, a reduced LMR probe, a null-move search's
    // children alternate). Only used to reduce more in LMR.
    Value negamax(Position& pos, int depth, Value alpha, Value beta, int ply,
                  bool pvNode, bool cutNode, bool prevWasNull) {
        ++nodes_;
        if (ply > seldepth_) seldepth_ = ply;
        pvLength_[ply] = ply;

        if (should_stop())
            return alpha;

        if (ply > 0 && pos.is_draw())
            return VALUE_DRAW;

        if (depth <= 0)
            return quiescence(pos, alpha, beta, ply, pvNode);

        const Key   key        = pos.key();
        const Value alphaOrig  = alpha;
        const bool  inCheck    = pos.checkers() != 0;

        // ---- Transposition-table probe ---------------------------------
        // Copy every field we need right away: the entry pointer can be
        // overwritten by stores made during deeper recursion.
        TTEntry*   tte     = nullptr;
        const bool ttHit   = TT.probe(key, tte);
        const Move  ttMove  = ttHit ? tte->move() : MOVE_NONE;
        const Value ttValue = ttHit ? value_from_tt(tte->value(), ply) : VALUE_NONE;
        const Value ttEval  = ttHit ? tte->eval() : VALUE_NONE;
        const int   ttDepth = ttHit ? tte->depth() : -1;
        const Bound ttBound = ttHit ? tte->bound() : BOUND_NONE;

        // A sufficiently deep entry can cut, but only at a non-PV, non-root
        // node so the principal variation is always searched in full.
        if (ttHit && !pvNode && ply > 0 && ttDepth >= depth) {
            if (ttBound == BOUND_EXACT
                || (ttBound == BOUND_LOWER && ttValue >= beta)
                || (ttBound == BOUND_UPPER && ttValue <= alpha))
                return ttValue;
        }

        // Static eval of this node (side-relative), reused from the TT when
        // cached. Meaningless when in check, so left as VALUE_NONE there.
        const Value staticEval = inCheck            ? VALUE_NONE
                               : ttEval != VALUE_NONE ? ttEval
                                                      : Eval::evaluate(pos);
        StackEntry* const ss = &stack_[ply + STACK_OFFSET];
        ss->staticEval = staticEval;

        // "Improving": the static eval is higher than at our previous turn
        // (two plies up; four if we were in check then). In check there is no
        // eval and nothing is improving; with no earlier eval at all (both
        // ancestors in check, or near the root) assume improving, which is
        // the conservative choice for the pruning margins below.
        const bool improving =
              inCheck                             ? false
            : (ss - 2)->staticEval != VALUE_NONE ? staticEval > (ss - 2)->staticEval
            : (ss - 4)->staticEval != VALUE_NONE ? staticEval > (ss - 4)->staticEval
                                                 : true;

        // ---- Reverse futility pruning (static null move) ---------------
        // Cheapest pruner: no search at all. If the static eval is so far above
        // beta that even a generous per-ply margin can't drag it back, assume a
        // real move would also hold and return the eval (fail-soft). Shallow
        // depths only; never near a mate bound (a heuristic margin vs a mate
        // score is unsound).
        if (!pvNode
            && !inCheck
            && ply > 0
            && depth <= t_.rfpMaxDepth
            && std::abs(beta) < VALUE_MATE_IN_MAX_PLY
            && staticEval - t_.rfpMargin * (depth - improving) >= beta) {
            return staticEval;
        }

        // ---- Null-move pruning -----------------------------------------
        // If passing still fails high over beta from a reduced search, a real
        // move almost certainly would too -- prune. Guarded against PV nodes,
        // being in check, consecutive nulls, and zugzwang (no non-pawn material
        // for the side to move).
        if (!pvNode
            && !inCheck
            && ply > 0
            && depth >= 3
            && !prevWasNull
            && pos.has_non_pawn_material(pos.side_to_move())
            && staticEval >= beta) {
            // Reduce more the further the static eval is above beta.
            const int R = t_.nmpBase + depth / t_.nmpDepthDiv
                        + std::min((staticEval - beta) / t_.nmpEvalDiv, t_.nmpEvalMax);
            ss->move     = MOVE_NONE;
            ss->moved    = NO_PIECE;
            ss->contHist = &td_.contHist.sentinel();
            pos.do_null_move();
            const Value v = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1,
                                     /*pvNode=*/false, !cutNode, /*prevWasNull=*/true);
            pos.undo_null_move();

            if (aborted_)
                return alpha;
            if (v >= beta)
                // A reduced search never proves a mate: don't propagate one.
                return (v >= VALUE_MATE_IN_MAX_PLY) ? beta : v;
        }

        // ---- Internal iterative reduction --------------------------------
        // No TT move at a deep node: ordering will be poor and the node is
        // probably not important, so search it one ply shallower (the next
        // iteration will find a TT move here).
        if (depth >= t_.iirMinDepth && !ttMove.is_ok())
            --depth;

        MovePicker mp(*this, pos, ttMove, killers_.primary(ply), killers_.secondary(ply),
                      ss, /*qsNoisyOnly=*/false);
        int moveScore = 0;

        Value bestScore = -VALUE_INFINITE;
        Move  bestMove  = MOVE_NONE;

        // Late move pruning: at shallow non-PV nodes, once this many moves
        // have been searched, the remaining quiets are skipped.
        const bool pruneNode = !pvNode && !inCheck;
        const int  lmpLimit  = (t_.lmpBase + depth * depth) / (2 - improving);
        // Futility: the static eval plus a depth-scaled margin cannot reach
        // alpha, so no quiet move is expected to either.
        const bool futile = pruneNode
                         && depth <= t_.fpMaxDepth
                         && staticEval + t_.fpBase + t_.fpMargin * depth <= alpha;
        bool       skipQuiets = false;
        int        movesSearched = 0;

        // Moves searched so far that did not cut, for the history maluses.
        const bool ttCapture = ttMove.is_ok() && !is_quiet_move(pos, ttMove);

        Move quietsTried[MoveList::CAPACITY];
        Move noisyTried[MoveList::CAPACITY];
        int  quietCount = 0;
        int  noisyCount = 0;

        for (Move m; (m = mp.next(moveScore)).is_ok(); ) {
            const int  newDepth = depth - 1;

            // Pre-move classification (parent position).
            const bool isQuiet  = is_quiet_move(pos, m);
            const Move k1       = killers_.primary(ply);
            const Move k2       = killers_.secondary(ply);
            const bool isKiller = (m == k1 || m == k2);
            const Piece moved   = pos.piece_on(m.from_sq());

            // ---- Shallow-depth pruning (non-PV, not in check) ----------
            // Only once some move has produced a non-losing score, so the
            // first move is always searched and a mated node is never
            // mis-scored by skipping its only defences.
            if (pruneNode && bestScore > -VALUE_MATE_IN_MAX_PLY) {
                if (isQuiet) {
                    if (skipQuiets)
                        continue;
                    // Late move pruning: every remaining quiet is skipped.
                    if (depth <= t_.lmpMaxDepth && movesSearched >= lmpLimit) {
                        skipQuiets = true;
                        mp.skip_quiets();
                        continue;
                    }
                } else if (depth <= t_.seePruneMaxDepth
                           && !see_ge(pos, m, -t_.seeNoisyMargin * depth * depth)) {
                    continue;   // SEE pruning: the capture loses too much
                }
            }

            // History of a quiet move (LMR adjustment). A noisy move is an LMR
            // candidate when it is a losing capture or its capture history
            // is negative.
            const int  quietHist = isQuiet ? quiet_history(pos, m, ss) : 0;
            const bool lmrNoisy  = !isQuiet
                && (moveScore < QS_BAD_CAPTURE_LIMIT
                    || td_.captureHistory.get(moved, m.to_sq(), captured_index(pos, m)) < 0);

            // Futility: no quiet is expected to reach alpha. SEE: the quiet
            // move hangs material. Both spare checking moves (see below).
            const bool quietPrunable =
                   isQuiet && pruneNode && bestScore > -VALUE_MATE_IN_MAX_PLY
                && (futile
                    || (depth <= t_.seePruneMaxDepth
                        && !see_ge(pos, m, -t_.seeQuietMargin * depth)));

            const bool givesCheck = pos.gives_check(m);

            // Futility / SEE pruning of a quiet move, unless it gives check.
            if (quietPrunable && !givesCheck)
                continue;

            ss->move     = m;
            ss->moved    = moved;
            ss->contHist = &td_.contHist.at(moved, m.to_sq());

            pos.make_move(m);

            // Principal Variation Search + late move reductions. The first move
            // gets the full window; later moves get a null-window scout (a non-PV
            // child), preceded by a reduced-depth probe when LMR applies. A
            // reduced probe that beats alpha is re-searched at full depth; a
            // scout that lands strictly inside (alpha, beta) is re-searched at
            // full depth with the full window (a PV child).
            Value score;
            if (movesSearched == 0) {
                score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, pvNode,
                                 /*cutNode=*/!pvNode && !cutNode, /*prevWasNull=*/false);
            } else {
                int R = 0;
                if (depth >= t_.lmrMinDepth
                    && movesSearched >= t_.lmrMinMoveIndex
                    && !inCheck
                    && (isQuiet || lmrNoisy)) {
                    const int d = std::min(depth, LMR_TABLE_SIZE - 1);
                    const int n = std::min(movesSearched + 1, LMR_TABLE_SIZE - 1);
                    // Fixed point: 1024 == one ply; Pct tunables are 1/100 ply.
                    int r = lmrTable_[isQuiet][d][n];
                    if (pvNode)     r -= t_.lmrPvPct * 1024 / 100;
                    if (givesCheck) r -= t_.lmrCheckPct * 1024 / 100;
                    if (isKiller)   r -= t_.lmrKillerPct * 1024 / 100;
                    if (!improving) r += t_.lmrImprovingPct * 1024 / 100;
                    if (ttCapture)  r += t_.lmrTtCapturePct * 1024 / 100;
                    if (cutNode)    r += t_.lmrCutNodePct * 1024 / 100;
                    if (isQuiet)    r -= quietHist * 1024 / t_.lmrHistoryDiv;
                    // Round to plies; never extend, keep the probe at depth >= 1.
                    R = r > 0 ? (r + 512) / 1024 : 0;
                    R = std::min(R, newDepth - 1);
                }

                if (R > 0) {
                    score = -negamax(pos, newDepth - R, -alpha - 1, -alpha, ply + 1,
                                     /*pvNode=*/false, /*cutNode=*/true,
                                     /*prevWasNull=*/false);
                    if (score > alpha)
                        score = -negamax(pos, newDepth, -alpha - 1, -alpha, ply + 1,
                                         /*pvNode=*/false, !cutNode, /*prevWasNull=*/false);
                } else {
                    score = -negamax(pos, newDepth, -alpha - 1, -alpha, ply + 1,
                                     /*pvNode=*/false, !cutNode, /*prevWasNull=*/false);
                }

                if (pvNode && score > alpha && score < beta)
                    score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, /*pvNode=*/true,
                                     /*cutNode=*/false, /*prevWasNull=*/false);
            }

            pos.unmake_move(m);
            ++movesSearched;

            if (aborted_)
                return alpha;

            if (score > bestScore) {
                bestScore = score;
                bestMove  = m;

                if (score > alpha) {
                    alpha = score;
                    pvTable_[ply][ply] = m;
                    for (int next = ply + 1; next < pvLength_[ply + 1]; ++next)
                        pvTable_[ply][next] = pvTable_[ply + 1][next];
                    pvLength_[ply] = pvLength_[ply + 1];

                    if (score >= beta) {
                        update_histories(pos, ss, ply, depth, m, isQuiet,
                                         quietsTried, quietCount,
                                         noisyTried, noisyCount);
                        break;                     // fail-high cutoff
                    }
                }
            }

            if (isQuiet)
                quietsTried[quietCount++] = m;
            else
                noisyTried[noisyCount++] = m;
        }

        // No legal move: checkmate or stalemate.
        if (mp.yielded() == 0)
            return inCheck ? static_cast<Value>(-(VALUE_MATE - ply))
                           : VALUE_DRAW;

        // ---- Single store at node exit ---------------------------------
        const Bound bound = (bestScore >= beta)     ? BOUND_LOWER
                          : (bestScore > alphaOrig) ? BOUND_EXACT
                                                    : BOUND_UPPER;
        TT.store(key, bestMove, bestScore, staticEval, depth, bound, ply);

        return bestScore;
    }
};

} // namespace

void Search::clear() {
    g_mainThreadData.clear();
}

Search::Result Search::search(Position& pos, const Search::Limits& limits,
                              std::atomic<bool>& stop) {
    Searcher s(stop, g_mainThreadData);
    const Search::Result r = s.iterate(pos, limits, /*printInfo=*/true);
    // Exactly one bestmove per `go`: iterate always yields a legal fallback
    // move (or MOVE_NONE -> "0000" for a terminal position). Line-atomic emit.
    sync_cout("bestmove " + to_uci(r.bestMove) + "\n");
    return r;
}

// Test helpers. search_fixed starts every call from cleared history so a
// test's result does not depend on which searches ran before it (the TT is
// the caller's to clear). search_nodes deliberately keeps history, like a
// real game search: datagen calls it once per move and resets the state
// with Search::clear() at the start of each game.
Search::Result Search::search_fixed(Position& pos, int depth, std::atomic<bool>& stop) {
    Search::clear();
    Searcher s(stop, g_mainThreadData);
    Search::Limits limits;
    limits.depth = depth;
    return s.iterate(pos, limits, /*printInfo=*/false);
}

Search::Result Search::search_fixed(Position& pos, int depth) {
    std::atomic<bool> never{ false };
    return search_fixed(pos, depth, never);
}

Search::Result Search::search_fixed(Position& pos, int depth, bool useAspiration) {
    std::atomic<bool> never{ false };
    Search::clear();
    Searcher s(never, g_mainThreadData);
    Search::Limits limits;
    limits.depth      = depth;
    limits.aspiration = useAspiration;
    return s.iterate(pos, limits, /*printInfo=*/false);
}

Search::Result Search::search_nodes(Position& pos, std::uint64_t nodeLimit,
                                    std::atomic<bool>& stop) {
    Searcher s(stop, g_mainThreadData);
    Search::Limits limits;
    limits.nodes = nodeLimit;
    return s.iterate(pos, limits, /*printInfo=*/false);
}

Search::Result Search::search_nodes(Position& pos, std::uint64_t nodeLimit) {
    std::atomic<bool> never{ false };
    return search_nodes(pos, nodeLimit, never);
}

namespace {

// Fixed, reproducible bench set: the Stage 1 perft positions plus a handful of
// representative middlegame / endgame positions.
const char* const kBenchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "rnbqkb1r/pp1p1ppp/2p5/4P3/2B5/8/PPP1NnPP/RNBQK2R w KQkq - 0 6",
    "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
    "r1bqk2r/ppp2ppp/2n2n2/2bpp3/4P3/2NP1N2/PPP2PPP/R1BQKB1R w KQkq - 0 5",
    "2rq1rk1/pp1bppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/2KR1B1R w - - 0 10",
    "8/8/4k3/8/8/4K3/4P3/8 w - - 0 1",
    "8/8/8/2k5/2pP4/8/B7/4K3 b - d3 0 1",
    "8/pp3p1k/2p2q1p/3r1P2/5R2/7P/P1P1QP2/7K b - - 2 30",
    "r3r1k1/ppqb1ppp/8/4p1NQ/8/2P5/PP3PPP/R3R1K1 b - - 0 1",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "8/8/1P6/5pr1/8/4R3/7k/2K5 w - - 0 1",
};

} // namespace

std::uint64_t Search::bench(int depth) {
    std::atomic<bool> never{ false };
    std::uint64_t     totalNodes = 0;

    // Reproducible signature: a fixed-size table, cleared (together with the
    // persistent history) before each position so the node count is
    // independent of any prior `setoption Hash` state or earlier searches.
    TT.resize(16);

    const auto start = Clock::now();
    int idx = 0;
    for (const char* fen : kBenchFens) {
        Position pos;
        pos.set_fen(fen);

        TT.clear();
        Search::clear();

        Searcher s(never, g_mainThreadData);
        Limits   limits;
        limits.depth = depth;
        const Result r = s.iterate(pos, limits, /*printInfo=*/false);

        totalNodes += s.nodes();
        std::cout << "position " << ++idx << " depth " << depth
                  << " score " << static_cast<int>(r.score)
                  << " nodes " << s.nodes() << std::endl;
    }

    const std::int64_t ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    const std::uint64_t nps =
        (ms > 0) ? (totalNodes * 1000ULL) / static_cast<std::uint64_t>(ms) : totalNodes * 1000ULL;

    std::cout << "===========================\n";
    std::cout << "Total time (ms) : " << ms << '\n';
    std::cout << "Nodes searched  : " << totalNodes << '\n';
    std::cout << "Nodes/second    : " << nps << std::endl;

    return totalNodes;
}

int Search::lmr_base_reduction(const Tunables& t, bool quiet, int depth, int moveNumber) {
    if (depth < 1 || moveNumber < 1)
        return 0;
    const double base = (quiet ? t.lmrQuietBasePct : t.lmrNoisyBasePct) / 100.0;
    const double div  = (quiet ? t.lmrQuietDivPct  : t.lmrNoisyDivPct)  / 100.0;
    const double r    = base + std::log(depth) * std::log(moveNumber) / div;
    return static_cast<int>(std::lround(r * 1024.0));
}

const Search::Tunables& Search::active_tunables() {
    return g_tunables;
}

void Search::set_tunables(const Tunables& t) {
    g_tunables = t;
}
