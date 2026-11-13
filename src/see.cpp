#include "see.hpp"

#include "attacks.hpp"
#include "bitboard.hpp"
#include "position.hpp"

// ===========================================================================
// see.cpp - swap-algorithm static exchange evaluation (see see.hpp).
//
// Rather than building the whole gain list, the threshold form tracks a
// single running balance `swap` and a parity bit `res` (1 == the side to move
// at the root currently meets the threshold). Each iteration lets the side to
// move recapture with its least valuable attacker; the loop stops as soon as
// that side cannot improve on the current outcome, so the result is the
// minimax value of the exchange compared against the threshold.
// ===========================================================================

bool see_ge(const Position& pos, Move m, int threshold) {
    const MoveType mt = m.type_of();
    if (mt == CASTLING)
        return 0 >= threshold;

    const Square from = m.from_sq();
    const Square to   = m.to_sq();
    const Color  us   = pos.side_to_move();

    // Material gained by the first capture, and the value of the piece that
    // then stands on `to` (and can be captured next).
    int       captured = 0;
    PieceType onTarget = type_of(pos.piece_on(from));
    Bitboard  occupied = pos.pieces();

    if (mt == EN_PASSANT) {
        captured = SEE::value(PAWN);
        // The captured pawn sits behind `to` from the mover's point of view.
        occupied ^= square_bb(to - pawn_push(us));
    } else if (pos.piece_on(to) != NO_PIECE) {
        captured = SEE::value(type_of(pos.piece_on(to)));
    }

    if (mt == PROMOTION) {
        onTarget  = m.promotion_type();
        captured += SEE::value(onTarget) - SEE::value(PAWN);
    }

    // Even if the opponent cannot recapture, we fall short of the threshold.
    int swap = captured - threshold;
    if (swap < 0)
        return false;

    // A legal king move is never recaptured.
    if (onTarget == KING)
        return true;

    // Even if the opponent wins our piece for free, we still meet it.
    swap = SEE::value(onTarget) - swap;
    if (swap <= 0)
        return true;

    occupied ^= square_bb(from) | square_bb(to);
    Bitboard attackers = pos.attackers_to(to, occupied);

    const Bitboard diagSliders = pos.pieces(BISHOP, QUEEN);
    const Bitboard orthSliders = pos.pieces(ROOK, QUEEN);

    Color stm = us;
    int   res = 1;

    while (true) {
        stm = ~stm;
        attackers &= occupied;

        const Bitboard stmAttackers = attackers & pos.pieces(stm);
        if (!stmAttackers)
            break;

        res ^= 1;

        Bitboard bb;
        if ((bb = stmAttackers & pos.pieces(PAWN))) {
            if ((swap = SEE::value(PAWN) - swap) < res)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & diagSliders;
        } else if ((bb = stmAttackers & pos.pieces(KNIGHT))) {
            if ((swap = SEE::value(KNIGHT) - swap) < res)
                break;
            occupied ^= square_bb(lsb(bb));
        } else if ((bb = stmAttackers & pos.pieces(BISHOP))) {
            if ((swap = SEE::value(BISHOP) - swap) < res)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & diagSliders;
        } else if ((bb = stmAttackers & pos.pieces(ROOK))) {
            if ((swap = SEE::value(ROOK) - swap) < res)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= rook_attacks(to, occupied) & orthSliders;
        } else if ((bb = stmAttackers & pos.pieces(QUEEN))) {
            if ((swap = SEE::value(QUEEN) - swap) < res)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= (bishop_attacks(to, occupied) & diagSliders)
                       | (rook_attacks(to, occupied)   & orthSliders);
        } else {
            // Only the king is left: it may capture only if the opponent has
            // no attacker remaining (otherwise the capture would be illegal).
            return (attackers & ~pos.pieces(stm)) ? (res ^ 1) : res;
        }
    }

    return res != 0;
}
