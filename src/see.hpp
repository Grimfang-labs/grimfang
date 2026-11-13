#pragma once

#include "move.hpp"
#include "types.hpp"

// ===========================================================================
// see.hpp - static exchange evaluation (SEE).
//
// see_ge(pos, m, threshold) answers "does the capture sequence started by `m`
// on its target square win at least `threshold` centipawns for the side to
// move?", assuming both sides always recapture with their least valuable
// attacker and may stop whenever continuing would lose material. This is the
// classic swap algorithm with x-ray attackers: when a pawn, bishop, rook or
// queen leaves the line, sliders behind it are revealed.
//
// Conventions / simplifications:
//   * `m` must be legal in `pos` (as produced by the move generator).
//   * Pins are ignored: a pinned piece may take part in the exchange.
//   * A king may only capture when the square is no longer defended.
//   * En passant: the captured pawn (not on the target square) is removed
//     from the occupancy, so a slider behind it on the file is revealed.
//   * Promotions are scored with the promoted piece standing on the target
//     square (gain = captured + promo - pawn). Later recaptures that would
//     themselves promote are valued as plain pawn captures (conservative).
//   * Castling never wins or loses material (SEE value 0).
// ===========================================================================

class Position;

namespace SEE {

// Piece values used by the exchange (king effectively infinite).
constexpr int VALUE[PIECE_TYPE_NB] = { 100, 320, 330, 500, 900, 20000 };

inline int value(PieceType pt) { return VALUE[pt]; }

} // namespace SEE

// True iff the static exchange value of `m` is >= threshold.
bool see_ge(const Position& pos, Move m, int threshold);
