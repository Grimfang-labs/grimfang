#pragma once

#include <bit>
#include <string>

#include "types.hpp"

// ===========================================================================
// bitboard.hpp - bit manipulation primitives, square/file/rank masks, and the
// precomputed between/line geometry tables used for pins, checks and blocking.
// ===========================================================================

namespace BB {

// Initialise the geometry tables (between_bb / line_bb). Must be called once at
// startup, after Attacks::init() is NOT required (this only uses ray walks
// implemented locally). Safe to call standalone.
void init();

} // namespace BB

// ---------------------------------------------------------------------------
// Constant single-square / file / rank masks
// ---------------------------------------------------------------------------
constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = FileABB << 1;
constexpr Bitboard FileCBB = FileABB << 2;
constexpr Bitboard FileDBB = FileABB << 3;
constexpr Bitboard FileEBB = FileABB << 4;
constexpr Bitboard FileFBB = FileABB << 5;
constexpr Bitboard FileGBB = FileABB << 6;
constexpr Bitboard FileHBB = FileABB << 7;

constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << (8 * 1);
constexpr Bitboard Rank3BB = Rank1BB << (8 * 2);
constexpr Bitboard Rank4BB = Rank1BB << (8 * 3);
constexpr Bitboard Rank5BB = Rank1BB << (8 * 4);
constexpr Bitboard Rank6BB = Rank1BB << (8 * 5);
constexpr Bitboard Rank7BB = Rank1BB << (8 * 6);
constexpr Bitboard Rank8BB = Rank1BB << (8 * 7);

constexpr Bitboard square_bb(Square s) { return 1ULL << s; }
constexpr Bitboard file_bb(File f)     { return FileABB << f; }
constexpr Bitboard rank_bb(Rank r)     { return Rank1BB << (8 * r); }
constexpr Bitboard file_bb(Square s)   { return file_bb(file_of(s)); }
constexpr Bitboard rank_bb(Square s)   { return rank_bb(rank_of(s)); }

// ---------------------------------------------------------------------------
// Bit counting / scanning
// ---------------------------------------------------------------------------
constexpr int popcount(Bitboard b) { return std::popcount(b); }

inline Square lsb(Bitboard b) {
    return static_cast<Square>(std::countr_zero(b));
}
inline Square msb(Bitboard b) {
    return static_cast<Square>(63 ^ std::countl_zero(b));
}

// Pop and return the least-significant set bit's square.
inline Square pop_lsb(Bitboard& b) {
    const Square s = lsb(b);
    b &= b - 1;
    return s;
}

constexpr bool more_than_one(Bitboard b) { return (b & (b - 1)) != 0; }

// ---------------------------------------------------------------------------
// Direction shifts with file-wrap masking.
// ---------------------------------------------------------------------------
template<Direction D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH)      return  b << 8;
    else if constexpr (D == SOUTH) return  b >> 8;
    else if constexpr (D == EAST)  return (b & ~FileHBB) << 1;
    else if constexpr (D == WEST)  return (b & ~FileABB) >> 1;
    else if constexpr (D == NORTH_EAST) return (b & ~FileHBB) << 9;
    else if constexpr (D == NORTH_WEST) return (b & ~FileABB) << 7;
    else if constexpr (D == SOUTH_EAST) return (b & ~FileHBB) >> 7;
    else if constexpr (D == SOUTH_WEST) return (b & ~FileABB) >> 9;
    else return 0;
}

// ---------------------------------------------------------------------------
// Geometry tables (defined in bitboard.cpp)
// ---------------------------------------------------------------------------
namespace detail {
extern Bitboard BetweenBB[SQ_NB][SQ_NB];
extern Bitboard LineBB[SQ_NB][SQ_NB];
} // namespace detail

// Squares strictly between a and b along a shared rank/file/diagonal
// (exclusive of both endpoints). Empty if not aligned.
inline Bitboard between_bb(Square a, Square b) { return detail::BetweenBB[a][b]; }

// Full line through a and b (the whole rank/file/diagonal), or empty if not
// aligned. Includes both endpoints.
inline Bitboard line_bb(Square a, Square b) { return detail::LineBB[a][b]; }

// True if a, b, c are collinear (share a rank/file/diagonal).
inline bool aligned(Square a, Square b, Square c) {
    return (line_bb(a, b) & square_bb(c)) != 0;
}

// ---------------------------------------------------------------------------
// Debug pretty-printer (rank 8 at the top).
// ---------------------------------------------------------------------------
std::string pretty(Bitboard b);
