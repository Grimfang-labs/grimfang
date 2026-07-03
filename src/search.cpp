#include "search.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "eval.hpp"
#include "history.hpp"
#include "killers.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "sync.hpp"
#include "timeman.hpp"
#include "tt.hpp"

// ===========================================================================
// search.cpp - negamax alpha-beta + quiescence + iterative deepening.
//
// Move ordering (Stage 3): the transposition-table move first, then captures
// by MVV-LVA, then quiet moves in generation order. A shared transposition
// table provides depth-preferred cutoffs at non-PV nodes and a best-move hint
// everywhere. Mate scores are made node-relative on store / re-rooted on probe.
// Quiet moves that cause a beta cutoff are remembered as killers (two per ply)
// and ordered ahead of generic quiets; a butterfly history table (rewarded on
// quiet cutoffs, penalised for quiets tried first) orders the remaining quiets.
// No SEE yet.
//
// PV-node classification: a node is a PV node iff it is reached by always
// following the first (best) move from the root. TT cutoffs are taken only at
// non-PV, non-root nodes so the principal variation is searched in full.
// ===========================================================================

namespace {

// Static piece values used purely for MVV-LVA move ordering.
constexpr int orderValue[PIECE_TYPE_NB] = { 100, 320, 330, 500, 900, 20000 };

constexpr int CAPTURE_BONUS   = 1'000'000;
constexpr int PROMOTION_BONUS =   900'000;
constexpr int TT_MOVE_BONUS   = 10'000'000;   // root previous-best move

// Reverse futility pruning (static null move): only at shallow depths, and
// only when the static eval clears beta by a per-ply margin.
constexpr int RFP_MAX_DEPTH = 8;
constexpr int RFP_MARGIN    = 100;   // centipawns per ply of remaining depth

// Late move reductions: search late quiet moves at reduced depth first, verify
// at full depth if the reduced scout beats alpha (PVS re-search unchanged).
constexpr int LMR_MIN_DEPTH      = 3;
constexpr int LMR_MIN_MOVE_INDEX   = 3;   // 0-based: 4th move onward

// Poll the stop conditions roughly every this many nodes.
constexpr std::uint64_t POLL_INTERVAL = 2048;

using Clock = std::chrono::steady_clock;

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
    explicit Searcher(std::atomic<bool>& stop) : stop_(stop) {}

    Search::Result iterate(Position& pos, const Search::Limits& limits, bool printInfo) {
        nodes_     = 0;
        seldepth_  = 0;
        aborted_   = false;
        startTime_ = Clock::now();

        // One search == one generation. Entries from earlier searches stay in
        // the table but age out under replacement. Killers and history are
        // search-local and start empty.
        TT.new_search();
        killers_.clear();
        history_.clear();

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

        for (int depth = 1; depth <= maxDepth; ++depth) {
            // Soft limit: don't start a new iteration if we are unlikely to
            // finish it within the budget.
            if (useTime_ && depth > 1 && elapsed_ms() >= softMs_)
                break;

            const Value score =
                negamax(pos, depth, -VALUE_INFINITE, VALUE_INFINITE, 0,
                        /*pvNode=*/true, /*prevWasNull=*/false);

            const bool haveMove = pvLength_[0] > 0;
            if (aborted_) {
                // Keep the result from the last fully completed iteration; the
                // very first iteration still yields a usable move if it found one.
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

    Clock::time_point startTime_;
    bool              useTime_ = false;
    std::int64_t      softMs_  = 0;
    std::int64_t      hardMs_  = 0;

    Move pvTable_[MAX_PLY][MAX_PLY];
    int  pvLength_[MAX_PLY] = { 0 };

    KillerTable  killers_;
    HistoryTable history_;

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

    int score_move(const Position& pos, Move m, Move ttMove, Move k1, Move k2) const {
        if (ttMove.is_ok() && m == ttMove)
            return TT_MOVE_BONUS;

        const MoveType mt   = m.type_of();
        const Square   to   = m.to_sq();
        const bool     isEp = (mt == EN_PASSANT);
        const bool     isCapture = isEp || (pos.piece_on(to) != NO_PIECE);

        int score = 0;
        if (isCapture) {
            const PieceType victim   = isEp ? PAWN : type_of(pos.piece_on(to));
            const PieceType attacker = type_of(pos.piece_on(m.from_sq()));
            score = CAPTURE_BONUS + orderValue[victim] * 16 - orderValue[attacker];
        }
        if (mt == PROMOTION)
            score += PROMOTION_BONUS + orderValue[m.promotion_type()];

        // Quiet move (no capture/promotion signal): killers first, then order
        // the rest by history. History values (|.| < MAX_HISTORY) sit far below
        // the killer band, so the total order stays TT > captures > K1 > K2 >
        // history-sorted quiets.
        if (score == 0) {
            const int kb = killer_bonus(m, k1, k2);
            if (kb)
                return kb;
            return history_.get(pos.side_to_move(), m);
        }

        return score;
    }

    // Stable insertion sort, descending by score (ties keep generation order).
    // k1/k2 are this ply's killers (MOVE_NONE to disable, e.g. in quiescence).
    void order_moves(const Position& pos, MoveList& list, Move ttMove,
                     Move k1, Move k2) const {
        int scores[MoveList::CAPACITY];
        for (int i = 0; i < list.count; ++i)
            scores[i] = score_move(pos, list.moves[i], ttMove, k1, k2);

        for (int i = 1; i < list.count; ++i) {
            const Move m  = list.moves[i];
            const int  sc = scores[i];
            int j = i - 1;
            while (j >= 0 && scores[j] < sc) {
                list.moves[j + 1] = list.moves[j];
                scores[j + 1]     = scores[j];
                --j;
            }
            list.moves[j + 1] = m;
            scores[j + 1]     = sc;
        }
    }

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

    // ---- Quiescence ----------------------------------------------------
    Value quiescence(Position& pos, Value alpha, Value beta, int ply) {
        ++nodes_;
        if (ply > seldepth_) seldepth_ = ply;
        if (should_stop())   return alpha;
        if (ply >= MAX_PLY - 1)
            return Eval::evaluate(pos);

        const Value standPat = Eval::evaluate(pos);
        if (standPat >= beta)
            return beta;
        if (standPat > alpha)
            alpha = standPat;

        MoveList caps;
        generate_captures(pos, caps);
        order_moves(pos, caps, MOVE_NONE, MOVE_NONE, MOVE_NONE);

        for (int i = 0; i < caps.count; ++i) {
            const Move m = caps.moves[i];
            pos.make_move(m);
            const Value score = -quiescence(pos, -beta, -alpha, ply + 1);
            pos.unmake_move(m);

            if (aborted_)
                return alpha;
            if (score >= beta)
                return beta;
            if (score > alpha)
                alpha = score;
        }
        return alpha;
    }

    // ---- Negamax alpha-beta (fail-soft) --------------------------------
    Value negamax(Position& pos, int depth, Value alpha, Value beta, int ply,
                  bool pvNode, bool prevWasNull) {
        ++nodes_;
        if (ply > seldepth_) seldepth_ = ply;
        pvLength_[ply] = ply;

        if (should_stop())
            return alpha;

        if (ply > 0 && pos.is_draw())
            return VALUE_DRAW;

        if (depth <= 0)
            return quiescence(pos, alpha, beta, ply);

        const Key   key        = pos.key();
        const Value alphaOrig  = alpha;
        const bool  inCheck    = pos.checkers() != 0;

        // ---- Transposition-table probe ---------------------------------
        TTEntry*   tte    = nullptr;
        const bool ttHit  = TT.probe(key, tte);
        const Move ttMove = ttHit ? tte->move() : MOVE_NONE;

        // A sufficiently deep entry can cut, but only at a non-PV, non-root
        // node so the principal variation is always searched in full.
        if (ttHit && !pvNode && ply > 0 && tte->depth() >= depth) {
            const Value ttValue = value_from_tt(tte->value(), ply);
            const Bound b       = tte->bound();
            if (b == BOUND_EXACT
                || (b == BOUND_LOWER && ttValue >= beta)
                || (b == BOUND_UPPER && ttValue <= alpha))
                return ttValue;
        }

        // Static eval of this node (side-relative). Computed once for non-check
        // nodes; used by null-move pruning now and by RFP next rung. Meaningless
        // when in check, so left as VALUE_NONE there.
        const Value staticEval = inCheck ? VALUE_NONE : Eval::evaluate(pos);

        // ---- Reverse futility pruning (static null move) ---------------
        // Cheapest pruner: no search at all. If the static eval is so far above
        // beta that even a generous per-ply margin can't drag it back, assume a
        // real move would also hold and return the eval (fail-soft). Shallow
        // depths only; never near a mate bound (a heuristic margin vs a mate
        // score is unsound).
        if (!pvNode
            && !inCheck
            && ply > 0
            && depth <= RFP_MAX_DEPTH
            && std::abs(beta) < VALUE_MATE_IN_MAX_PLY
            && staticEval - RFP_MARGIN * depth >= beta) {
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
            const int R = 3 + depth / 3;
            pos.do_null_move();
            const Value v = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1,
                                     /*pvNode=*/false, /*prevWasNull=*/true);
            pos.undo_null_move();

            if (aborted_)
                return alpha;
            if (v >= beta)
                // A reduced search never proves a mate: don't propagate one.
                return (v >= VALUE_MATE_IN_MAX_PLY) ? beta : v;
        }

        MoveList list;
        generate_legal(pos, list);

        if (list.count == 0)
            return inCheck ? static_cast<Value>(-(VALUE_MATE - ply))
                           : VALUE_DRAW;

        order_moves(pos, list, ttMove, killers_.primary(ply), killers_.secondary(ply));

        Value bestScore = -VALUE_INFINITE;
        Move  bestMove  = MOVE_NONE;

        for (int i = 0; i < list.count; ++i) {
            const Move m = list.moves[i];
            const int  newDepth = depth - 1;

            // Pre-move classification (parent position).
            const bool isQuiet  = is_quiet_move(pos, m);
            const bool isTtMove = ttMove.is_ok() && m == ttMove;
            const Move k1       = killers_.primary(ply);
            const Move k2       = killers_.secondary(ply);
            const bool isKiller = (m == k1 || m == k2);

            pos.make_move(m);
            const bool givesCheck = pos.checkers() != 0;

            // Principal Variation Search + late move reductions. The first move
            // gets the full window; later moves get a null-window scout (a non-PV
            // child). LMR inserts a reduced-depth probe before the full-depth
            // scout when eligible; when R==0 the path is identical to pre-LMR PVS.
            // A scout that lands strictly inside (alpha, beta) is re-searched at
            // full depth with the full window (a PV child).
            Value score;
            if (i == 0) {
                score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, pvNode,
                                 /*prevWasNull=*/false);
            } else {
                int R = 0;
                if (depth >= LMR_MIN_DEPTH
                    && i >= LMR_MIN_MOVE_INDEX
                    && isQuiet
                    && !isTtMove
                    && !inCheck
                    && !givesCheck) {
                    R = 1;
                    if (depth >= 6) R += 1;
                    if (i >= 12)     R += 1;
                    if (pvNode)      R = std::max(R - 1, 0);
                    if (isKiller)    R = std::max(R - 1, 0);
                    R = std::min(R, newDepth - 1);
                }

                if (R > 0) {
                    score = -negamax(pos, newDepth - R, -alpha - 1, -alpha, ply + 1,
                                     /*pvNode=*/false, /*prevWasNull=*/false);
                    if (score > alpha)
                        score = -negamax(pos, newDepth, -alpha - 1, -alpha, ply + 1,
                                         /*pvNode=*/false, /*prevWasNull=*/false);
                } else {
                    score = -negamax(pos, newDepth, -alpha - 1, -alpha, ply + 1,
                                     /*pvNode=*/false, /*prevWasNull=*/false);
                }

                if (score > alpha && score < beta)
                    score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, pvNode,
                                     /*prevWasNull=*/false);
            }

            pos.unmake_move(m);

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
                        // Remember a quiet refutation for sibling nodes at this
                        // ply, and update history: reward the cutoff move,
                        // penalise the quiets tried before it (history gravity).
                        // Captures/promotions already order well via MVV-LVA and
                        // are left untouched.
                        if (is_quiet_move(pos, m)) {
                            killers_.update(ply, m);

                            const Color stm   = pos.side_to_move();
                            const int   bonus = history_bonus(depth);
                            history_.update(stm, m, bonus);
                            for (int j = 0; j < i; ++j)
                                if (is_quiet_move(pos, list.moves[j]))
                                    history_.update(stm, list.moves[j], -bonus);
                        }
                        break;                     // fail-high cutoff
                    }
                }
            }
        }

        // ---- Single store at node exit ---------------------------------
        const Bound bound = (bestScore >= beta)     ? BOUND_LOWER
                          : (bestScore > alphaOrig) ? BOUND_EXACT
                                                    : BOUND_UPPER;
        // Static eval is not cached at internal nodes yet (RFP/NMP rung).
        TT.store(key, bestMove, bestScore, VALUE_NONE, depth, bound, ply);

        return bestScore;
    }
};

} // namespace

Search::Result Search::search(Position& pos, const Search::Limits& limits,
                              std::atomic<bool>& stop) {
    Searcher s(stop);
    const Search::Result r = s.iterate(pos, limits, /*printInfo=*/true);
    // Exactly one bestmove per `go`: iterate always yields a legal fallback
    // move (or MOVE_NONE -> "0000" for a terminal position). Line-atomic emit.
    sync_cout("bestmove " + to_uci(r.bestMove) + "\n");
    return r;
}

Search::Result Search::search_fixed(Position& pos, int depth, std::atomic<bool>& stop) {
    Searcher s(stop);
    Search::Limits limits;
    limits.depth = depth;
    return s.iterate(pos, limits, /*printInfo=*/false);
}

Search::Result Search::search_fixed(Position& pos, int depth) {
    std::atomic<bool> never{ false };
    return search_fixed(pos, depth, never);
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

    // Reproducible signature: a fixed-size table, cleared before each position
    // so the node count is independent of any prior `setoption Hash` state.
    TT.resize(16);

    const auto start = Clock::now();
    int idx = 0;
    for (const char* fen : kBenchFens) {
        Position pos;
        pos.set_fen(fen);

        TT.clear();

        Searcher s(never);
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
