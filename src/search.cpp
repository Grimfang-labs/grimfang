#include "search.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "eval.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "timeman.hpp"
#include "tt.hpp"

// ===========================================================================
// search.cpp - negamax alpha-beta + quiescence + iterative deepening.
//
// Move ordering (Stage 3): the transposition-table move first, then captures
// by MVV-LVA, then quiet moves in generation order. A shared transposition
// table provides depth-preferred cutoffs at non-PV nodes and a best-move hint
// everywhere. Mate scores are made node-relative on store / re-rooted on probe.
// No killers, history or SEE yet.
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
        // the table but age out under replacement.
        TT.new_search();

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
                negamax(pos, depth, -VALUE_INFINITE, VALUE_INFINITE, 0, /*pvNode=*/true);

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

    int score_move(const Position& pos, Move m, Move ttMove) const {
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

        return score;
    }

    // Stable insertion sort, descending by score (ties keep generation order).
    void order_moves(const Position& pos, MoveList& list, Move ttMove) const {
        int scores[MoveList::CAPACITY];
        for (int i = 0; i < list.count; ++i)
            scores[i] = score_move(pos, list.moves[i], ttMove);

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

        std::cout << "info depth " << depth
                  << " seldepth " << seldepth_
                  << " score " << score_to_uci(score)
                  << " nodes " << nodes_
                  << " nps " << nps
                  << " hashfull " << TT.hashfull()
                  << " time " << ms
                  << " pv";
        for (int i = 0; i < pvLength_[0]; ++i)
            std::cout << ' ' << to_uci(pvTable_[0][i]);
        std::cout << std::endl;
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
        order_moves(pos, caps, MOVE_NONE);

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
    Value negamax(Position& pos, int depth, Value alpha, Value beta, int ply, bool pvNode) {
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

        MoveList list;
        generate_legal(pos, list);

        if (list.count == 0)
            return pos.checkers() ? static_cast<Value>(-(VALUE_MATE - ply))
                                  : VALUE_DRAW;

        order_moves(pos, list, ttMove);

        Value bestScore = -VALUE_INFINITE;
        Move  bestMove  = MOVE_NONE;

        for (int i = 0; i < list.count; ++i) {
            const Move m = list.moves[i];
            pos.make_move(m);
            // Only the first (best) move of a PV node continues the PV.
            const bool childPv = pvNode && (i == 0);
            const Value score  = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, childPv);
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

                    if (score >= beta)
                        break;                     // fail-high cutoff
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
    std::cout << "bestmove " << to_uci(r.bestMove) << std::endl;
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
