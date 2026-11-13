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
// a score table over quiet moves, rewarded on a quiet beta cutoff and
// penalised for the quiets that were tried first and failed ("gravity").
//
// Persistent: owned by the search's per-thread state (ThreadData in
// search.cpp), kept across searches of one game and cleared by
// Search::clear() (ucinewgame, bench, datagen game start).
// ===========================================================================

// Gravity target: repeated bonuses converge on +/-MAX_HISTORY instead of
// overflowing, so it must sit well inside the int16_t range.
constexpr int MAX_HISTORY       = 16384;

// Clamp on the depth-scaled bonus (depth*depth), keeping single updates small.
constexpr int HISTORY_BONUS_CAP = 1600;

// Depth-scaled quiet-cutoff bonus, clamped to a cap. The multiplier is a
// percentage (100 == 1.0) so it can be exposed as an integer UCI option; the
// defaults (multPct=100, cap=HISTORY_BONUS_CAP) reproduce the original
// min(depth*depth, 1600) exactly.
inline int history_bonus(int depth, int multPct = 100, int cap = HISTORY_BONUS_CAP) {
    const int b = depth * depth * multPct / 100;
    return b < cap ? b : cap;
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

// ---------------------------------------------------------------------------
// Compact piece index for history tables: white P..K -> 0..5, black -> 6..11.
// ---------------------------------------------------------------------------
constexpr int HISTORY_PIECE_NB = 12;

inline int history_piece_index(Piece pc) {
    return static_cast<int>(color_of(pc)) * 6 + static_cast<int>(type_of(pc));
}

// ---------------------------------------------------------------------------
// Continuation history: how well a quiet move (piece, to) did as a reply to
// an earlier move (piece', to') played one or two plies before. The outer
// [piece'][to'] index selects a PieceToHistory slice; the search stack keeps
// a pointer to the slice of the move played at each ply, so the lookup for
// the current move is one indexed load per continuation ply.
// ---------------------------------------------------------------------------
using PieceToHistory = std::int16_t[HISTORY_PIECE_NB][SQ_NB];

struct ContinuationHistory {
    PieceToHistory table[HISTORY_PIECE_NB][SQ_NB];

    void clear() { std::memset(table, 0, sizeof(table)); }

    PieceToHistory& at(Piece pc, Square to) {
        return table[history_piece_index(pc)][to];
    }

    // Slice used for plies whose move has no continuation (null move, plies
    // below the root). A white pawn can never move to a1, so no real move
    // maps here; updates skip such plies, so it stays all zero.
    PieceToHistory& sentinel() { return table[0][A1]; }
};

// ---------------------------------------------------------------------------
// Capture history: [moved piece][to][captured piece type]. Slot NO_CAPTURE is
// used by non-capturing promotions.
// ---------------------------------------------------------------------------
struct CaptureHistory {
    static constexpr int NO_CAPTURE = PIECE_TYPE_NB;   // 6

    std::int16_t table[HISTORY_PIECE_NB][SQ_NB][PIECE_TYPE_NB + 1];

    void clear() { std::memset(table, 0, sizeof(table)); }

    void update(Piece pc, Square to, int captured, int delta) {
        HistoryTable::apply(table[history_piece_index(pc)][to][captured], delta);
    }

    int get(Piece pc, Square to, int captured) const {
        return table[history_piece_index(pc)][to][captured];
    }
};
