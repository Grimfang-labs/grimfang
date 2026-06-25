#include "search.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "eval.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "timeman.hpp"

// ===========================================================================
// search.cpp - negamax alpha-beta + quiescence + iterative deepening.
//
// Move ordering (Stage 2): captures first by MVV-LVA, quiet moves in
// generation order, and at the root the previous iteration's best move first.
// No transposition table, killers, history or SEE.
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
        prevBest_  = MOVE_NONE;
        startTime_ = Clock::now();

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

            const Value score = negamax(pos, depth, -VALUE_INFINITE, VALUE_INFINITE, 0);

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
            prevBest_        = result.bestMove;

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
    Move               prevBest_   = MOVE_NONE;

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

    // ---- Negamax alpha-beta --------------------------------------------
    Value negamax(Position& pos, int depth, Value alpha, Value beta, int ply) {
        ++nodes_;
        if (ply > seldepth_) seldepth_ = ply;
        pvLength_[ply] = ply;

        if (should_stop())
            return alpha;

        if (ply > 0 && pos.is_draw())
            return VALUE_DRAW;

        if (depth <= 0)
            return quiescence(pos, alpha, beta, ply);

        MoveList list;
        generate_legal(pos, list);

        if (list.count == 0)
            return pos.checkers() ? static_cast<Value>(-(VALUE_MATE - ply))
                                  : VALUE_DRAW;

        order_moves(pos, list, ply == 0 ? prevBest_ : MOVE_NONE);

        for (int i = 0; i < list.count; ++i) {
            const Move m = list.moves[i];
            pos.make_move(m);
            const Value score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
            pos.unmake_move(m);

            if (aborted_)
                return alpha;

            if (score >= beta)
                return beta;                       // fail-hard cutoff

            if (score > alpha) {
                alpha = score;
                pvTable_[ply][ply] = m;
                for (int next = ply + 1; next < pvLength_[ply + 1]; ++next)
                    pvTable_[ply][next] = pvTable_[ply + 1][next];
                pvLength_[ply] = pvLength_[ply + 1];
            }
        }
        return alpha;
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
