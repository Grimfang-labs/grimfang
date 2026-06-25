#include "search.hpp"

#include <algorithm>

#include "eval.hpp"
#include "movegen.hpp"
#include "position.hpp"

// ===========================================================================
// search.cpp - negamax alpha-beta with quiescence (sub-step 2).
//
// Iterative deepening, the triangular PV and UCI `info` output build on the
// same Searcher in sub-step 3; here we expose only a fixed-depth entry point.
// ===========================================================================

namespace {

// Static piece values used purely for MVV-LVA move ordering (independent of the
// evaluation tables so ordering stays cheap and stable).
constexpr int orderValue[PIECE_TYPE_NB] = { 100, 320, 330, 500, 900, 20000 };

constexpr int CAPTURE_BONUS   = 1'000'000;
constexpr int PROMOTION_BONUS =   900'000;
constexpr int TT_MOVE_BONUS   = 10'000'000;   // root previous-best move

class Searcher {
public:
    explicit Searcher(std::atomic<bool>& stop) : stop_(stop) {}

    Search::Result run_fixed(Position& pos, int depth) {
        nodes_   = 0;
        prevBest_ = MOVE_NONE;

        const Value score = negamax(pos, depth, -VALUE_INFINITE, VALUE_INFINITE, 0);

        Search::Result r;
        r.score    = score;
        r.depth    = depth;
        r.nodes    = nodes_;
        r.bestMove = (pvLength_[0] > 0) ? pvTable_[0][0] : MOVE_NONE;
        return r;
    }

private:
    std::atomic<bool>& stop_;
    std::uint64_t      nodes_ = 0;
    Move               prevBest_ = MOVE_NONE;

    Move pvTable_[MAX_PLY][MAX_PLY];
    int  pvLength_[MAX_PLY] = { 0 };

    // ---- MVV-LVA / root ordering ---------------------------------------
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
                list.moves[j + 1]  = list.moves[j];
                scores[j + 1]      = scores[j];
                --j;
            }
            list.moves[j + 1] = m;
            scores[j + 1]     = sc;
        }
    }

    bool out_of_nodes() {
        return stop_.load(std::memory_order_relaxed);
    }

    // ---- Quiescence ----------------------------------------------------
    Value quiescence(Position& pos, Value alpha, Value beta, int ply) {
        ++nodes_;
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

            if (out_of_nodes())
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
        pvLength_[ply] = ply;

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

            if (out_of_nodes())
                return alpha;

            if (score >= beta)
                return beta;                       // fail-hard cutoff

            if (score > alpha) {
                alpha = score;
                // Record into the triangular PV table.
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

Search::Result Search::search_fixed(Position& pos, int depth, std::atomic<bool>& stop) {
    Searcher s(stop);
    return s.run_fixed(pos, depth);
}

Search::Result Search::search_fixed(Position& pos, int depth) {
    std::atomic<bool> never{ false };
    return search_fixed(pos, depth, never);
}
