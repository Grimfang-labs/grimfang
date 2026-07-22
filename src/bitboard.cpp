#include "bitboard.hpp"

#include <cassert>
#include <cstdlib>

// ===========================================================================
// bitboard.cpp - geometry tables and pretty printer.
//
// between_bb / line_bb are built from a local slow ray walk so this module has
// no dependency on attacks.cpp (keeps the init ordering simple: BB::init() can
// run first).
// ===========================================================================

// Storage for the geometry tables declared in bitboard.hpp (global `detail`).
namespace detail {
Bitboard BetweenBB[SQ_NB][SQ_NB];
Bitboard LineBB[SQ_NB][SQ_NB];
} // namespace detail

namespace {

// The eight sliding directions; sliders only ever travel a subset of these.
constexpr Direction RookDirs[4]   = { NORTH, EAST, SOUTH, WEST };
constexpr Direction BishopDirs[4] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };

// Walk a single ray from `from` in step direction `d`, stopping at the board
// edge. `occ` blockers are inclusive (the blocker square is added then we stop).
// Destination is validated before any square_bb shift (same GCC 13 -O3 hazard
// as attacks.cpp::ray_attacks: direction-group edge checks can be dropped).
Bitboard ray_attacks(Square from, Direction d, Bitboard occ) {
    Bitboard attacks = 0;
    Square   s       = from;

    while (true) {
        assert(is_ok(s));
        const int to = static_cast<int>(s) + static_cast<int>(d);
        if (to < 0 || to > 63) break;
        if (std::abs(static_cast<int>(file_of(static_cast<Square>(to))) -
                     static_cast<int>(file_of(s))) > 1)
            break;

        s = static_cast<Square>(to);
        assert(is_ok(s));
        attacks |= square_bb(s);
        if (occ & square_bb(s)) break;   // hit a blocker (inclusive)
    }
    return attacks;
}

Bitboard slider_attacks(Square s, const Direction dirs[4], Bitboard occ) {
    Bitboard a = 0;
    for (int i = 0; i < 4; ++i)
        a |= ray_attacks(s, dirs[i], occ);
    return a;
}

} // namespace

namespace BB {

void init() {
    using detail::BetweenBB;
    using detail::LineBB;

    for (int a = A1; a <= H8; ++a) {
        for (int b = A1; b <= H8; ++b) {
            BetweenBB[a][b] = 0;
            LineBB[a][b]    = 0;
        }
    }

    for (int a = A1; a <= H8; ++a) {
        const Square sa = static_cast<Square>(a);

        for (const Direction* dirs : { RookDirs, BishopDirs }) {
            for (int i = 0; i < 4; ++i) {
                const Direction d = dirs[i];

                // Walk outward; for each reachable square b on this ray, the
                // squares strictly between sa and b are exactly the ray-attacks
                // from sa that stop just before b.
                Bitboard between = 0;
                Square   s       = sa;

                while (true) {
                    assert(is_ok(s));
                    const int to = static_cast<int>(s) + static_cast<int>(d);
                    if (to < 0 || to > 63) break;
                    if (std::abs(static_cast<int>(file_of(static_cast<Square>(to))) -
                                 static_cast<int>(file_of(s))) > 1)
                        break;

                    s = static_cast<Square>(to);
                    assert(is_ok(s));
                    BetweenBB[a][s] = between;     // squares strictly between
                    between |= square_bb(s);       // include s for the next hop
                }
            }
        }
    }

    // Full line through any two aligned squares: union of both ray directions
    // from a, plus both endpoints. Built from the unblocked slider attacks.
    for (int a = A1; a <= H8; ++a) {
        const Square sa = static_cast<Square>(a);
        for (int b = A1; b <= H8; ++b) {
            const Square sb = static_cast<Square>(b);
            if (a == b) continue;

            const Bitboard rookA = slider_attacks(sa, RookDirs, 0);
            const Bitboard bishA = slider_attacks(sa, BishopDirs, 0);

            if (rookA & square_bb(sb)) {
                LineBB[a][b] = (rookA & slider_attacks(sb, RookDirs, 0))
                               | square_bb(sa) | square_bb(sb);
            } else if (bishA & square_bb(sb)) {
                LineBB[a][b] = (bishA & slider_attacks(sb, BishopDirs, 0))
                               | square_bb(sa) | square_bb(sb);
            }
        }
    }
}

} // namespace BB

std::string pretty(Bitboard b) {
    std::string s = "+---+---+---+---+---+---+---+---+\n";
    for (int r = RANK_8; r >= RANK_1; --r) {
        for (int f = FILE_A; f <= FILE_H; ++f) {
            const Square sq = make_square(static_cast<File>(f), static_cast<Rank>(r));
            s += (b & square_bb(sq)) ? "| X " : "|   ";
        }
        s += "| ";
        s += static_cast<char>('1' + r);
        s += "\n+---+---+---+---+---+---+---+---+\n";
    }
    s += "  a   b   c   d   e   f   g   h\n";
    return s;
}
