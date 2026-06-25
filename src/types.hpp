#pragma once

#include <cstdint>

// ===========================================================================
// types.hpp - fundamental enums, typedefs and small helpers.
//
// Square mapping is Little-Endian Rank-File (LERF):
//   a1 = 0, b1 = 1, ... h1 = 7, a2 = 8, ... h8 = 63.
//   file_of(sq) = sq & 7,  rank_of(sq) = sq >> 3.
// ===========================================================================

using Bitboard = std::uint64_t;
using Key      = std::uint64_t;

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------
enum Color : int {
    WHITE,
    BLACK,
    COLOR_NB = 2
};

constexpr Color operator~(Color c) {
    return static_cast<Color>(c ^ BLACK);
}

// ---------------------------------------------------------------------------
// Piece type / Piece
// ---------------------------------------------------------------------------
enum PieceType : int {
    PAWN,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,
    PIECE_TYPE_NB = 6
};

// Pieces are laid out so that color_of = piece >> 3 and type_of = piece & 7.
// White pieces 0..5, black pieces 8..13. NO_PIECE = 14. PIECE_NB = 16 for
// array sizing convenience.
enum Piece : int {
    W_PAWN = 0, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 8, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    NO_PIECE = 14,
    PIECE_NB = 16
};

constexpr Piece make_piece(Color c, PieceType pt) {
    return static_cast<Piece>((c << 3) + pt);
}

constexpr PieceType type_of(Piece pc) {
    return static_cast<PieceType>(pc & 7);
}

constexpr Color color_of(Piece pc) {
    // Only valid for actual pieces (not NO_PIECE).
    return static_cast<Color>(pc >> 3);
}

// ---------------------------------------------------------------------------
// Squares, files, ranks
// ---------------------------------------------------------------------------
enum Square : int {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    SQ_NB   = 64,
    SQ_NONE = 64
};

enum File : int {
    FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
    FILE_NB = 8
};

enum Rank : int {
    RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8,
    RANK_NB = 8
};

constexpr File file_of(Square s) { return static_cast<File>(s & 7); }
constexpr Rank rank_of(Square s) { return static_cast<Rank>(s >> 3); }

constexpr Square make_square(File f, Rank r) {
    return static_cast<Square>((r << 3) + f);
}

constexpr bool is_ok(Square s) { return s >= A1 && s <= H8; }

// Square as seen from a given color's perspective (flip vertically for BLACK).
constexpr Square relative_square(Color c, Square s) {
    return static_cast<Square>(s ^ (c * 56));
}

constexpr Rank relative_rank(Color c, Rank r) {
    return static_cast<Rank>(r ^ (c * 7));
}

constexpr Rank relative_rank(Color c, Square s) {
    return relative_rank(c, rank_of(s));
}

// ---------------------------------------------------------------------------
// Directions (LERF deltas)
// ---------------------------------------------------------------------------
enum Direction : int {
    NORTH =  8,
    EAST  =  1,
    SOUTH = -8,
    WEST  = -1,

    NORTH_EAST = NORTH + EAST,   //  9
    SOUTH_EAST = SOUTH + EAST,   // -7
    SOUTH_WEST = SOUTH + WEST,   // -9
    NORTH_WEST = NORTH + WEST    //  7
};

constexpr Square operator+(Square s, Direction d) {
    return static_cast<Square>(static_cast<int>(s) + static_cast<int>(d));
}
constexpr Square operator-(Square s, Direction d) {
    return static_cast<Square>(static_cast<int>(s) - static_cast<int>(d));
}
constexpr Square& operator+=(Square& s, Direction d) { return s = s + d; }
constexpr Square& operator-=(Square& s, Direction d) { return s = s - d; }

// Pawn push direction for a color.
constexpr Direction pawn_push(Color c) { return c == WHITE ? NORTH : SOUTH; }

// ---------------------------------------------------------------------------
// Castling rights (4-bit flag set)
// ---------------------------------------------------------------------------
enum CastlingRights : int {
    NO_CASTLING = 0,
    WHITE_OO    = 1,
    WHITE_OOO   = 2,
    BLACK_OO    = 4,
    BLACK_OOO   = 8,

    KING_SIDE      = WHITE_OO  | BLACK_OO,
    QUEEN_SIDE     = WHITE_OOO | BLACK_OOO,
    WHITE_CASTLING = WHITE_OO  | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO  | BLACK_OOO,
    ANY_CASTLING   = WHITE_CASTLING | BLACK_CASTLING,
    CASTLING_NB    = 16
};

constexpr CastlingRights operator|(CastlingRights a, CastlingRights b) {
    return static_cast<CastlingRights>(static_cast<int>(a) | static_cast<int>(b));
}
constexpr CastlingRights operator&(CastlingRights a, CastlingRights b) {
    return static_cast<CastlingRights>(static_cast<int>(a) & static_cast<int>(b));
}
constexpr CastlingRights& operator|=(CastlingRights& a, CastlingRights b) { return a = a | b; }
constexpr CastlingRights& operator&=(CastlingRights& a, CastlingRights b) { return a = a & b; }
constexpr CastlingRights operator~(CastlingRights a) {
    return static_cast<CastlingRights>(~static_cast<int>(a) & ANY_CASTLING);
}

constexpr CastlingRights king_side_rights(Color c)  { return c == WHITE ? WHITE_OO  : BLACK_OO; }
constexpr CastlingRights queen_side_rights(Color c) { return c == WHITE ? WHITE_OOO : BLACK_OOO; }
