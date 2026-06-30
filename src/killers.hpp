#pragma once

#include "move.hpp"
#include "position.hpp"
#include "score.hpp"

// ===========================================================================
// killers.hpp - killer-move heuristic (Stage 3, rung 3.3).
//
// A killer is a *quiet* move that produced a beta cutoff at a given ply. Quiet
// refutations tend to repeat across sibling positions at the same ply, so we
// keep two per ply and order them ahead of generic quiets (but after the TT
// move and captures/promotions). Killers are search-local: cleared at the
// start of each search, never persisted (that is history's job, rung 3.4).
// ===========================================================================

// Ordering bonuses placed strictly between captures/promotions (>= ~900k) and
// generic quiet moves (0), so the priority is: TT move, captures/promotions,
// killer 1, killer 2, then the rest.
constexpr int KILLER1_BONUS = 800'000;
constexpr int KILLER2_BONUS = 790'000;

// A "quiet" move for ordering/killer purposes: neither a capture (including en
// passant) nor a promotion. Classified against the CURRENT position.
inline bool is_quiet_move(const Position& pos, Move m) {
    const MoveType mt = m.type_of();
    if (mt == PROMOTION || mt == EN_PASSANT)
        return false;
    return pos.piece_on(m.to_sq()) == NO_PIECE;
}

// Ordering bonus for a quiet move given a ply's two killers (0 if it matches
// neither, or the killer slot is empty).
inline int killer_bonus(Move m, Move killer1, Move killer2) {
    if (killer1.is_ok() && m == killer1) return KILLER1_BONUS;
    if (killer2.is_ok() && m == killer2) return KILLER2_BONUS;
    return 0;
}

// Two killers per ply: slot 0 is the most recent.
struct KillerTable {
    Move slots[MAX_PLY][2];

    void clear() {
        for (int p = 0; p < MAX_PLY; ++p)
            slots[p][0] = slots[p][1] = MOVE_NONE;
    }

    // Record a quiet cutoff move at `ply`. The caller guarantees `m` is quiet.
    // Slot 0 always holds the most recent killer; an equal move is not
    // duplicated into slot 1.
    void update(int ply, Move m) {
        if (slots[ply][0] != m) {
            slots[ply][1] = slots[ply][0];
            slots[ply][0] = m;
        }
    }

    Move primary(int ply)   const { return slots[ply][0]; }
    Move secondary(int ply) const { return slots[ply][1]; }
};
