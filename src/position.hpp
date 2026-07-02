#pragma once

#include <string>
#include <vector>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "move.hpp"
#include "types.hpp"
#include "zobrist.hpp"

// ===========================================================================
// position.hpp - board representation and the make/unmake engine.
//
// Hybrid representation:
//   * byColor_[2]            - occupancy per color
//   * byType_[6]             - occupancy per piece type (color-agnostic)
//   * board_[64]             - mailbox for O(1) piece lookup
// plus side-to-move, castling rights, ep square, halfmove clock, fullmove
// number and an incrementally-updated Zobrist key. Undo information is kept on
// a per-move StateInfo stack so unmake_move is exact.
// ===========================================================================

// Saved per move so unmake_move can restore state exactly.
struct StateInfo {
    CastlingRights castling;
    Square         ep;
    int            rule50;
    Piece          captured;
    Key            key;
};

class Position {
public:
    Position() { clear(); }

    // -- FEN ---------------------------------------------------------------
    void        set_fen(const std::string& fen);
    std::string fen() const;

    static Position startpos();

    // -- Make / unmake -----------------------------------------------------
    void make_move(Move m);
    void unmake_move(Move m);

    // Null move (pass): flip the side to move, clear the ep square, bump the
    // halfmove clock, and update the Zobrist key. Pushes the same undo/history
    // stacks a real move does so repetition detection stays consistent. Only
    // legal to call when the side to move is NOT in check.
    void do_null_move();
    void undo_null_move();

    // -- Board queries -----------------------------------------------------
    Bitboard pieces() const { return byColor_[WHITE] | byColor_[BLACK]; }
    Bitboard pieces(Color c) const { return byColor_[c]; }
    Bitboard pieces(PieceType pt) const { return byType_[pt]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColor_[c] & byType_[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byType_[a] | byType_[b]; }
    Bitboard pieces(Color c, PieceType a, PieceType b) const {
        return byColor_[c] & (byType_[a] | byType_[b]);
    }

    Piece  piece_on(Square s) const { return board_[s]; }
    bool   empty(Square s) const { return board_[s] == NO_PIECE; }
    Square king_sq(Color c) const { return lsb(pieces(c, KING)); }

    // True when `c` has at least one non-pawn, non-king piece (N/B/R/Q). Used
    // as the zugzwang guard for null-move pruning.
    bool has_non_pawn_material(Color c) const {
        return (byColor_[c] & (byType_[KNIGHT] | byType_[BISHOP]
                             | byType_[ROOK]   | byType_[QUEEN])) != 0;
    }

    Color          side_to_move() const { return sideToMove_; }
    Square         ep_square() const { return ep_; }
    CastlingRights castling_rights() const { return castling_; }
    int            halfmove_clock() const { return rule50_; }
    int            fullmove_number() const { return fullMove_; }
    Key            key() const { return key_; }

    bool can_castle(CastlingRights cr) const { return (castling_ & cr) != 0; }

    // -- Draw detection ----------------------------------------------------
    // Fifty-move rule OR repetition over the retained key history. A single
    // earlier occurrence within the irreversible window counts (the standard
    // "twofold within the search tree" shortcut).
    bool is_draw() const;

    // -- Attack / legality queries ----------------------------------------
    // All pieces (either color) attacking `s` given occupancy `occ`.
    Bitboard attackers_to(Square s, Bitboard occ) const;
    Bitboard attackers_to(Square s) const { return attackers_to(s, pieces()); }

    // Is `s` attacked by any piece of color `by`, given occupancy `occ`?
    bool is_square_attacked(Square s, Color by, Bitboard occ) const;

    // Enemy pieces giving check to the side-to-move's king.
    Bitboard checkers() const;

    // Own pieces of color `c` absolutely pinned to their king.
    Bitboard pinned(Color c) const;

    // Squares attacked by color `by` over occupancy `occ` (used for king-safety
    // and castling tests). The caller is responsible for adjusting `occ`
    // (e.g. removing the friendly king) when needed.
    Bitboard attacks_by(Color by, Bitboard occ) const;

    // Recompute the Zobrist key from scratch (used by debug assertions/tests).
    Key compute_key() const;

private:
    void clear();
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);

    // True when an enemy pawn of `stm` can actually capture en passant on `ep`
    // (used to gate the Zobrist ep component for exact key equality).
    bool ep_capturable(Square ep, Color stm) const;

    Bitboard byColor_[COLOR_NB];
    Bitboard byType_[PIECE_TYPE_NB];
    Piece    board_[SQ_NB];

    Color          sideToMove_;
    CastlingRights castling_;
    Square         ep_;
    int            rule50_;
    int            fullMove_;
    Key            key_;

    std::vector<StateInfo> states_;

    // Zobrist keys of every position reached since game setup (back() == key_).
    // Seeded by set_fen and grown/shrunk by make/unmake so repetition detection
    // also sees positions that preceded the search.
    std::vector<Key> keyHistory_;
};
