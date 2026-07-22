#include "attacks.hpp"

#include <cassert>
#include <cstdlib>

#if defined(USE_PEXT)
  #include <immintrin.h>
#endif

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
// Destination is validated before any square_bb shift: GCC 13 -O3 previously
// dropped a direction-group RANK_1 edge check, letting s go negative and
// producing 1ULL << (negative) UB (x86 masks the count → phantom high bits).
Bitboard ray_attacks(Square from, Direction d, Bitboard occ) {
    Bitboard attacks = 0;
    Square   s       = from;

    while (true) {
        assert(is_ok(s));
        const int to = static_cast<int>(s) + static_cast<int>(d);
        if (to < 0 || to > 63) break;
        // Reject file wrap (a-file WEST / h-file EAST stepping onto the wrong file).
        if (std::abs(static_cast<int>(file_of(static_cast<Square>(to))) -
                     static_cast<int>(file_of(s))) > 1)
            break;

        s = static_cast<Square>(to);
        assert(is_ok(s));
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

// ===========================================================================
// Pass B - fancy magic bitboards.
//
// Per square we store a relevant-occupancy mask, a magic multiplier, a shift,
// and a pointer into a shared backing array. The index is
//   ((occ & mask) * magic) >> shift
// (or a BMI2 PEXT extraction when USE_PEXT is defined). Magics are searched at
// init with a fixed-seed PRNG, so builds are reproducible, and the resulting
// tables are verified against the slow reference for every blocker subset.
// ===========================================================================

// TEMP DIAGNOSTIC: external linkage so tests/perft_tests.cpp can dump the first
// magic/slow mismatch. Revert with the perft_tests.cpp diagnostic.
struct Magic {
    Bitboard  mask;
    Bitboard  magic;
    Bitboard* attacks;
    unsigned  shift;
};

Magic RookMagics[SQ_NB];
Magic BishopMagics[SQ_NB];

// Standard fancy-magic backing-array sizes (sum of 1 << relevant_bits).
Bitboard RookTable[102400];
Bitboard BishopTable[5248];

namespace {

// Relevant occupancy mask: slider rays excluding the board-edge squares (a
// blocker on the far edge never changes which squares are attacked).
Bitboard rook_mask(Square s) {
    Bitboard m = 0;
    const int f = file_of(s), r = rank_of(s);
    for (int rr = r + 1; rr <= 6; ++rr) m |= square_bb(make_square(static_cast<File>(f), static_cast<Rank>(rr)));
    for (int rr = r - 1; rr >= 1; --rr) m |= square_bb(make_square(static_cast<File>(f), static_cast<Rank>(rr)));
    for (int ff = f + 1; ff <= 6; ++ff) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(r)));
    for (int ff = f - 1; ff >= 1; --ff) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(r)));
    return m;
}

Bitboard bishop_mask(Square s) {
    Bitboard m = 0;
    const int f = file_of(s), r = rank_of(s);
    for (int ff = f + 1, rr = r + 1; ff <= 6 && rr <= 6; ++ff, ++rr) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(rr)));
    for (int ff = f + 1, rr = r - 1; ff <= 6 && rr >= 1; ++ff, --rr) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(rr)));
    for (int ff = f - 1, rr = r + 1; ff >= 1 && rr <= 6; --ff, ++rr) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(rr)));
    for (int ff = f - 1, rr = r - 1; ff >= 1 && rr >= 1; --ff, --rr) m |= square_bb(make_square(static_cast<File>(ff), static_cast<Rank>(rr)));
    return m;
}

inline unsigned magic_index(const Magic& m, Bitboard occ) {
#if defined(USE_PEXT)
    return static_cast<unsigned>(_pext_u64(occ, m.mask));
#else
    return static_cast<unsigned>(((occ & m.mask) * m.magic) >> m.shift);
#endif
}

// Deterministic sparse-bit PRNG used for the magic search.
class MagicRng {
public:
    explicit MagicRng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    std::uint64_t sparse() { return next() & next() & next(); }
private:
    std::uint64_t s_;
};

// Populate magics + backing slices for one slider kind.
void init_magic_kind(bool bishop, Magic magics[], Bitboard table[]) {
    Bitboard occ[4096];
    Bitboard ref[4096];
#if !defined(USE_PEXT)
    MagicRng rng(0xDEADBEEFCAFEF00DULL ^ (bishop ? 0x1ULL : 0x2ULL));
    unsigned epoch[4096] = {};
    unsigned gen = 0;
#endif

    int offset = 0;
    for (int sq = A1; sq <= H8; ++sq) {
        const Square s    = static_cast<Square>(sq);
        const Bitboard mask = bishop ? bishop_mask(s) : rook_mask(s);
        const int    bits = popcount(mask);
        const int    size = 1 << bits;

        Magic& m = magics[sq];
        m.mask    = mask;
        m.shift   = static_cast<unsigned>(64 - bits);
        m.attacks = table + offset;
        offset += size;

        // Enumerate every blocker subset of the mask (carry-rippler) and its
        // reference attack set.
        int n = 0;
        Bitboard b = 0;
        do {
            occ[n] = b;
            ref[n] = bishop ? bishop_attacks_slow(s, b) : rook_attacks_slow(s, b);
            ++n;
            b = (b - mask) & mask;
        } while (b);

#if defined(USE_PEXT)
        // PEXT needs no magic; fill directly.
        m.magic = 0;
        for (int i = 0; i < n; ++i)
            m.attacks[magic_index(m, occ[i])] = ref[i];
#else
        // Search for a collision-free (or constructively consistent) magic.
        while (true) {
            const Bitboard magic = rng.sparse();
            if (popcount((mask * magic) >> 56) < 6)
                continue;   // cheap quality filter

            m.magic = magic;
            ++gen;
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                const unsigned idx = magic_index(m, occ[i]);
                if (epoch[idx] != gen) {
                    epoch[idx]      = gen;
                    m.attacks[idx]  = ref[i];
                } else if (m.attacks[idx] != ref[i]) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                break;
        }
#endif
    }
}

// Verify magic results against the slow reference for every blocker subset.
void verify_magics() {
    for (int sq = A1; sq <= H8; ++sq) {
        const Square s = static_cast<Square>(sq);

        for (bool bishop : { false, true }) {
            const Bitboard mask = bishop ? BishopMagics[sq].mask : RookMagics[sq].mask;
            Bitboard b = 0;
            do {
                const Bitboard fast = bishop ? BishopMagics[sq].attacks[magic_index(BishopMagics[sq], b)]
                                             : RookMagics[sq].attacks[magic_index(RookMagics[sq], b)];
                const Bitboard slow = bishop ? bishop_attacks_slow(s, b)
                                             : rook_attacks_slow(s, b);
                if (fast != slow)
                    std::abort();   // magic table is wrong - fail the build/run loudly
                b = (b - mask) & mask;
            } while (b);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public fast surface (magic bitboards; identical results to the slow ref).
// ---------------------------------------------------------------------------
Bitboard bishop_attacks(Square s, Bitboard occ) {
    const Magic& m = BishopMagics[s];
    return m.attacks[magic_index(m, occ)];
}
Bitboard rook_attacks(Square s, Bitboard occ) {
    const Magic& m = RookMagics[s];
    return m.attacks[magic_index(m, occ)];
}
Bitboard queen_attacks(Square s, Bitboard occ) {
    return bishop_attacks(s, occ) | rook_attacks(s, occ);
}

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
    init_magic_kind(/*bishop=*/false, RookMagics, RookTable);
    init_magic_kind(/*bishop=*/true,  BishopMagics, BishopTable);
    verify_magics();
}

} // namespace Attacks
