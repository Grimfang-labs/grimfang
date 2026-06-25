#include "attacks.hpp"

#include <cassert>

// ===========================================================================
// attacks.cpp
//
// Pass A (this file, sub-step 1..3): slow ray-walk sliders + leaper tables.
// Pass B (sub-step 4): fancy magic bitboards, verified at init against the slow
// reference, exposed through the same bishop_attacks / rook_attacks surface.
// ===========================================================================

namespace {

// ---- Leaper tables -------------------------------------------------------
Bitboard KnightAttacks[SQ_NB];
Bitboard KingAttacks[SQ_NB];
Bitboard PawnAttacks[COLOR_NB][SQ_NB];

// ---- Slider ray directions ----------------------------------------------
constexpr Direction RookDirs[4]   = { NORTH, EAST, SOUTH, WEST };
constexpr Direction BishopDirs[4] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };

// Walk one ray, blockers inclusive (the blocker square is added then we stop).
Bitboard ray_attacks(Square from, Direction d, Bitboard occ) {
    Bitboard attacks = 0;
    Square   s       = from;

    while (true) {
        const File f = file_of(s);
        const Rank r = rank_of(s);
        if ((d == EAST || d == NORTH_EAST || d == SOUTH_EAST) && f == FILE_H) break;
        if ((d == WEST || d == NORTH_WEST || d == SOUTH_WEST) && f == FILE_A) break;
        if ((d == NORTH || d == NORTH_EAST || d == NORTH_WEST) && r == RANK_8) break;
        if ((d == SOUTH || d == SOUTH_EAST || d == SOUTH_WEST) && r == RANK_1) break;

        s += d;
        attacks |= square_bb(s);
        if (occ & square_bb(s)) break;
    }
    return attacks;
}

Bitboard slow_slider(Square s, const Direction dirs[4], Bitboard occ) {
    Bitboard a = 0;
    for (int i = 0; i < 4; ++i)
        a |= ray_attacks(s, dirs[i], occ);
    return a;
}

// Knight/king deltas as (file, rank) offsets to avoid wrap bugs.
struct Off { int df, dr; };

constexpr Off KnightOff[8] = {
    {  1,  2 }, {  2,  1 }, {  2, -1 }, {  1, -2 },
    { -1, -2 }, { -2, -1 }, { -2,  1 }, { -1,  2 }
};
constexpr Off KingOff[8] = {
    {  0,  1 }, {  1,  1 }, {  1,  0 }, {  1, -1 },
    {  0, -1 }, { -1, -1 }, { -1,  0 }, { -1,  1 }
};

void init_leapers() {
    for (int sq = A1; sq <= H8; ++sq) {
        const Square s = static_cast<Square>(sq);
        const int    f = file_of(s);
        const int    r = rank_of(s);

        Bitboard knight = 0, king = 0;
        for (const Off& o : KnightOff) {
            const int nf = f + o.df, nr = r + o.dr;
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                knight |= square_bb(make_square(static_cast<File>(nf), static_cast<Rank>(nr)));
        }
        for (const Off& o : KingOff) {
            const int nf = f + o.df, nr = r + o.dr;
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                king |= square_bb(make_square(static_cast<File>(nf), static_cast<Rank>(nr)));
        }
        KnightAttacks[sq] = knight;
        KingAttacks[sq]   = king;

        // Pawn captures: white attacks NE/NW, black attacks SE/SW.
        const Bitboard b = square_bb(s);
        PawnAttacks[WHITE][sq] = shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b);
        PawnAttacks[BLACK][sq] = shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Leaper accessors
// ---------------------------------------------------------------------------
Bitboard knight_attacks(Square s) { return KnightAttacks[s]; }
Bitboard king_attacks(Square s)   { return KingAttacks[s]; }
Bitboard pawn_attacks(Color c, Square s) { return PawnAttacks[c][s]; }

Bitboard pawn_attacks_bb(Color c, Bitboard pawns) {
    return c == WHITE
        ? shift<NORTH_WEST>(pawns) | shift<NORTH_EAST>(pawns)
        : shift<SOUTH_WEST>(pawns) | shift<SOUTH_EAST>(pawns);
}

// ---------------------------------------------------------------------------
// Slow reference sliders
// ---------------------------------------------------------------------------
Bitboard bishop_attacks_slow(Square s, Bitboard occ) { return slow_slider(s, BishopDirs, occ); }
Bitboard rook_attacks_slow(Square s, Bitboard occ)   { return slow_slider(s, RookDirs, occ); }

// ---------------------------------------------------------------------------
// Public fast surface (sub-step 1: routed to the slow reference; replaced by
// magic bitboards in sub-step 4 with identical results).
// ---------------------------------------------------------------------------
Bitboard bishop_attacks(Square s, Bitboard occ) { return bishop_attacks_slow(s, occ); }
Bitboard rook_attacks(Square s, Bitboard occ)   { return rook_attacks_slow(s, occ); }
Bitboard queen_attacks(Square s, Bitboard occ)  { return bishop_attacks(s, occ) | rook_attacks(s, occ); }

Bitboard attacks_bb(PieceType pt, Square s, Bitboard occ) {
    switch (pt) {
        case KNIGHT: return knight_attacks(s);
        case KING:   return king_attacks(s);
        case BISHOP: return bishop_attacks(s, occ);
        case ROOK:   return rook_attacks(s, occ);
        case QUEEN:  return queen_attacks(s, occ);
        default:     return 0;   // PAWN handled by caller
    }
}

namespace Attacks {

void init() {
    init_leapers();
    // Magic bitboard initialisation is added in sub-step 4.
}

} // namespace Attacks
