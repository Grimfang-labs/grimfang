#pragma once

#include <cstdint>
#include <string>

#include "types.hpp"

// ===========================================================================
// move.hpp - 16-bit packed move.
//
//   bits  0- 5 : from square (0..63)
//   bits  6-11 : to square   (0..63)
//   bits 12-13 : promotion piece type, stored as (PieceType - KNIGHT) -> 0..3
//   bits 14-15 : move type   { NORMAL, PROMOTION, EN_PASSANT, CASTLING }
//
// MOVE_NONE is the all-zero value (a "null" from==to==a1 NORMAL move never
// occurs in legal play, so 0 is safe as a sentinel).
// ===========================================================================

enum MoveType : std::uint16_t {
    NORMAL     = 0,
    PROMOTION  = 1 << 14,
    EN_PASSANT = 2 << 14,
    CASTLING   = 3 << 14
};

class Move {
public:
    Move() = default;
    constexpr explicit Move(std::uint16_t data) : data_(data) {}

    constexpr Move(Square from, Square to)
        : data_(static_cast<std::uint16_t>((from) | (to << 6))) {}

    // Typed factory. For PROMOTION the promo piece type must be in KNIGHT..QUEEN.
    template<MoveType T>
    static constexpr Move make(Square from, Square to, PieceType promo = KNIGHT) {
        return Move(static_cast<std::uint16_t>(
            T | ((promo - KNIGHT) << 12) | (to << 6) | from));
    }

    constexpr Square from_sq() const { return static_cast<Square>(data_ & 0x3F); }
    constexpr Square to_sq()   const { return static_cast<Square>((data_ >> 6) & 0x3F); }

    constexpr MoveType type_of() const {
        return static_cast<MoveType>(data_ & (3 << 14));
    }

    constexpr PieceType promotion_type() const {
        return static_cast<PieceType>(((data_ >> 12) & 3) + KNIGHT);
    }

    constexpr std::uint16_t raw() const { return data_; }

    constexpr bool is_ok() const { return data_ != 0; }   // not MOVE_NONE

    static constexpr Move none() { return Move(static_cast<std::uint16_t>(0)); }

    constexpr bool operator==(const Move& m) const { return data_ == m.data_; }
    constexpr bool operator!=(const Move& m) const { return data_ != m.data_; }

private:
    std::uint16_t data_ = 0;
};

constexpr Move MOVE_NONE = Move(static_cast<std::uint16_t>(0));

// e2e4 / e7e8q (lowercase promotion letter). Castling is rendered in standard
// king-target notation (e1g1 / e1c1), matching UCI for non-Chess960 play.
inline std::string to_uci(Move m) {
    if (!m.is_ok())
        return "0000";

    const Square from = m.from_sq();
    const Square to   = m.to_sq();

    std::string s;
    s += static_cast<char>('a' + file_of(from));
    s += static_cast<char>('1' + rank_of(from));
    s += static_cast<char>('a' + file_of(to));
    s += static_cast<char>('1' + rank_of(to));

    if (m.type_of() == PROMOTION) {
        constexpr char promoChar[PIECE_TYPE_NB] = { ' ', 'n', 'b', 'r', 'q', ' ' };
        s += promoChar[m.promotion_type()];
    }
    return s;
}
