#pragma once

#include "bitboard.hpp"
#include "types.hpp"

// ===========================================================================
// attacks.hpp - attack generation.
//
//   * Leapers (knight/king/pawn) come from precomputed tables.
//   * Sliders have a slow reference (ray walk) and a fast backend. The public
//     surface (bishop_attacks / rook_attacks / queen_attacks) hides which
//     backend is used; sub-step 1 routes it through the slow reference, and
//     magic bitboards are introduced in sub-step 4 without changing callers.
// ===========================================================================

namespace Attacks {

// Build all attack tables (and magics, once present). Requires BB::init() to
// have run first (used by the magic verification step). Idempotent.
void init();

} // namespace Attacks

// ---------------------------------------------------------------------------
// Leapers
// ---------------------------------------------------------------------------
Bitboard knight_attacks(Square s);
Bitboard king_attacks(Square s);
Bitboard pawn_attacks(Color c, Square s);

// All squares attacked by pawns of color c standing on the given set.
Bitboard pawn_attacks_bb(Color c, Bitboard pawns);

// ---------------------------------------------------------------------------
// Sliders - slow reference (always available, used by tests and magic verify)
// ---------------------------------------------------------------------------
Bitboard bishop_attacks_slow(Square s, Bitboard occ);
Bitboard rook_attacks_slow(Square s, Bitboard occ);

// ---------------------------------------------------------------------------
// Sliders - public fast surface
// ---------------------------------------------------------------------------
Bitboard bishop_attacks(Square s, Bitboard occ);
Bitboard rook_attacks(Square s, Bitboard occ);
Bitboard queen_attacks(Square s, Bitboard occ);

// Dispatch on piece type (PAWN handled separately by the caller).
Bitboard attacks_bb(PieceType pt, Square s, Bitboard occ);
