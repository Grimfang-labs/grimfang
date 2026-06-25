#include "position.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <sstream>

// ===========================================================================
// position.cpp
// ===========================================================================

namespace {

// Mapping from a square to the castling rights that are revoked when a piece
// leaves OR is captured on that square (king home squares and rook home
// squares). All other squares revoke nothing.
CastlingRights castlingMaskBySquare[SQ_NB];
bool           castlingMaskInit = false;

void init_castling_mask() {
    if (castlingMaskInit) return;
    for (int s = 0; s < SQ_NB; ++s)
        castlingMaskBySquare[s] = NO_CASTLING;

    castlingMaskBySquare[E1] = WHITE_OO | WHITE_OOO;
    castlingMaskBySquare[A1] = WHITE_OOO;
    castlingMaskBySquare[H1] = WHITE_OO;
    castlingMaskBySquare[E8] = BLACK_OO | BLACK_OOO;
    castlingMaskBySquare[A8] = BLACK_OOO;
    castlingMaskBySquare[H8] = BLACK_OO;
    castlingMaskInit = true;
}

constexpr char PieceChar[PIECE_NB + 1] = {
    'P', 'N', 'B', 'R', 'Q', 'K', '?', '?',
    'p', 'n', 'b', 'r', 'q', 'k', '.', '?', '\0'
};

Piece char_to_piece(char c) {
    switch (c) {
        case 'P': return W_PAWN;   case 'N': return W_KNIGHT;
        case 'B': return W_BISHOP; case 'R': return W_ROOK;
        case 'Q': return W_QUEEN;  case 'K': return W_KING;
        case 'p': return B_PAWN;   case 'n': return B_KNIGHT;
        case 'b': return B_BISHOP; case 'r': return B_ROOK;
        case 'q': return B_QUEEN;  case 'k': return B_KING;
        default:  return NO_PIECE;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Low-level board mutators (keep bitboards, mailbox and key in sync)
// ---------------------------------------------------------------------------
void Position::put_piece(Piece pc, Square s) {
    const Bitboard b = square_bb(s);
    board_[s] = pc;
    byColor_[color_of(pc)] |= b;
    byType_[type_of(pc)]   |= b;
    key_ ^= Zobrist::psq[pc][s];
}

void Position::remove_piece(Square s) {
    const Piece    pc = board_[s];
    const Bitboard b  = square_bb(s);
    byColor_[color_of(pc)] ^= b;
    byType_[type_of(pc)]   ^= b;
    board_[s] = NO_PIECE;
    key_ ^= Zobrist::psq[pc][s];
}

void Position::move_piece(Square from, Square to) {
    const Piece    pc = board_[from];
    const Bitboard b  = square_bb(from) | square_bb(to);
    byColor_[color_of(pc)] ^= b;
    byType_[type_of(pc)]   ^= b;
    board_[from] = NO_PIECE;
    board_[to]   = pc;
    key_ ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
}

void Position::clear() {
    for (int c = 0; c < COLOR_NB; ++c) byColor_[c] = 0;
    for (int t = 0; t < PIECE_TYPE_NB; ++t) byType_[t] = 0;
    for (int s = 0; s < SQ_NB; ++s) board_[s] = NO_PIECE;

    sideToMove_ = WHITE;
    castling_   = NO_CASTLING;
    ep_         = SQ_NONE;
    rule50_     = 0;
    fullMove_   = 1;
    key_        = 0;
    states_.clear();
    keyHistory_.clear();
}

// ---------------------------------------------------------------------------
// FEN parsing
// ---------------------------------------------------------------------------
void Position::set_fen(const std::string& fen) {
    init_castling_mask();
    clear();

    std::istringstream ss(fen);
    std::string placement, side, castle, ep;
    ss >> placement >> side >> castle >> ep;

    int half = 0, full = 1;
    if (!(ss >> half)) half = 0;
    if (!(ss >> full)) full = 1;

    // Piece placement: FEN starts at rank 8, file a.
    int file = FILE_A, rank = RANK_8;
    for (char c : placement) {
        if (c == '/') {
            file = FILE_A;
            --rank;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            file += c - '0';
        } else {
            const Piece pc = char_to_piece(c);
            if (pc != NO_PIECE && rank >= RANK_1 && file <= FILE_H) {
                put_piece(pc, make_square(static_cast<File>(file), static_cast<Rank>(rank)));
                ++file;
            }
        }
    }

    sideToMove_ = (side == "b") ? BLACK : WHITE;

    castling_ = NO_CASTLING;
    for (char c : castle) {
        switch (c) {
            case 'K': castling_ |= WHITE_OO;  break;
            case 'Q': castling_ |= WHITE_OOO; break;
            case 'k': castling_ |= BLACK_OO;  break;
            case 'q': castling_ |= BLACK_OOO; break;
            default: break;   // '-' or Shoichi/X-FEN unsupported in Stage 1
        }
    }

    ep_ = SQ_NONE;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && ep[1] >= '1' && ep[1] <= '8') {
        ep_ = make_square(static_cast<File>(ep[0] - 'a'), static_cast<Rank>(ep[1] - '1'));
    }

    rule50_   = half;
    fullMove_ = full;

    // Finalise the key with the non-piece components. The ep file is hashed
    // only when an enemy pawn could actually capture, so transpositions that
    // differ only by a non-capturable phantom ep square share one key.
    if (sideToMove_ == BLACK) key_ ^= Zobrist::side;
    key_ ^= Zobrist::castling[castling_];
    if (ep_capturable(ep_, sideToMove_)) key_ ^= Zobrist::enpassant[file_of(ep_)];

    // Seed the repetition history with the root position.
    keyHistory_.push_back(key_);
}

// ---------------------------------------------------------------------------
// FEN export
// ---------------------------------------------------------------------------
std::string Position::fen() const {
    std::ostringstream ss;

    for (int r = RANK_8; r >= RANK_1; --r) {
        int empties = 0;
        for (int f = FILE_A; f <= FILE_H; ++f) {
            const Square sq = make_square(static_cast<File>(f), static_cast<Rank>(r));
            const Piece  pc = board_[sq];
            if (pc == NO_PIECE) {
                ++empties;
            } else {
                if (empties) { ss << empties; empties = 0; }
                ss << PieceChar[pc];
            }
        }
        if (empties) ss << empties;
        if (r != RANK_1) ss << '/';
    }

    ss << ' ' << (sideToMove_ == WHITE ? 'w' : 'b') << ' ';

    if (castling_ == NO_CASTLING) {
        ss << '-';
    } else {
        if (castling_ & WHITE_OO)  ss << 'K';
        if (castling_ & WHITE_OOO) ss << 'Q';
        if (castling_ & BLACK_OO)  ss << 'k';
        if (castling_ & BLACK_OOO) ss << 'q';
    }

    ss << ' ';
    if (ep_ == SQ_NONE) {
        ss << '-';
    } else {
        ss << static_cast<char>('a' + file_of(ep_))
           << static_cast<char>('1' + rank_of(ep_));
    }

    ss << ' ' << rule50_ << ' ' << fullMove_;
    return ss.str();
}

Position Position::startpos() {
    Position p;
    p.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    return p;
}

// ---------------------------------------------------------------------------
// make_move / unmake_move
// ---------------------------------------------------------------------------
void Position::make_move(Move m) {
    const Color    us   = sideToMove_;
    const Color    them = ~us;
    const Square   from = m.from_sq();
    const Square   to   = m.to_sq();
    const MoveType mt   = m.type_of();
    const Piece    pc   = board_[from];
    const PieceType pt  = type_of(pc);

    StateInfo st;
    st.castling = castling_;
    st.ep       = ep_;
    st.rule50   = rule50_;
    st.key      = key_;          // exact pre-move key for restore
    st.captured = NO_PIECE;

    // Remove any current ep file from the key (re-added below on a new double
    // push). The side to move is the one that could have captured, so the same
    // capturability test that added the component now removes it.
    if (ep_ != SQ_NONE) {
        if (ep_capturable(ep_, us)) key_ ^= Zobrist::enpassant[file_of(ep_)];
        ep_ = SQ_NONE;
    }

    ++rule50_;

    // ---- Captures (including en passant) --------------------------------
    if (mt == EN_PASSANT) {
        const Square capSq = to - pawn_push(us);   // the pawn behind 'to'
        st.captured = board_[capSq];
        remove_piece(capSq);
        rule50_ = 0;
    } else if (board_[to] != NO_PIECE) {
        st.captured = board_[to];
        remove_piece(to);
        rule50_ = 0;
    }

    if (pt == PAWN) rule50_ = 0;

    // ---- Castling: relocate the rook (king is moved by move_piece below) -
    if (mt == CASTLING) {
        const bool   kingSide = to > from;
        const Square rookFrom = kingSide ? static_cast<Square>(to + 1)
                                         : static_cast<Square>(to - 2);
        const Square rookTo   = kingSide ? static_cast<Square>(to - 1)
                                         : static_cast<Square>(to + 1);
        move_piece(rookFrom, rookTo);
    }

    // ---- Move the moving piece ------------------------------------------
    move_piece(from, to);

    // ---- Promotion: swap the pawn for the promoted piece ----------------
    if (mt == PROMOTION) {
        remove_piece(to);
        put_piece(make_piece(us, m.promotion_type()), to);
    }

    // ---- New ep square on a pawn double push ----------------------------
    // The ep square is always set so move generation is unchanged; the key only
    // gains the ep component when the (new) side to move can actually capture.
    if (pt == PAWN && (to - from == 16 || from - to == 16)) {
        ep_ = static_cast<Square>((from + to) / 2);
        if (ep_capturable(ep_, them)) key_ ^= Zobrist::enpassant[file_of(ep_)];
    }

    // ---- Castling rights update -----------------------------------------
    const CastlingRights revoke =
        static_cast<CastlingRights>(castlingMaskBySquare[from] | castlingMaskBySquare[to]);
    if (castling_ & revoke) {
        key_ ^= Zobrist::castling[castling_];
        castling_ &= ~revoke;
        key_ ^= Zobrist::castling[castling_];
    }

    // ---- Side to move ----------------------------------------------------
    sideToMove_ = them;
    key_ ^= Zobrist::side;
    if (us == BLACK) ++fullMove_;

    states_.push_back(st);
    keyHistory_.push_back(key_);

    assert(key_ == compute_key());
}

void Position::unmake_move(Move m) {
    const StateInfo st = states_.back();
    states_.pop_back();
    keyHistory_.pop_back();

    sideToMove_ = ~sideToMove_;        // back to the mover
    const Color    us   = sideToMove_;
    const Square   from = m.from_sq();
    const Square   to   = m.to_sq();
    const MoveType mt   = m.type_of();

    if (us == BLACK) --fullMove_;

    // Undo promotion first (restore a pawn on 'to' before moving it back).
    if (mt == PROMOTION) {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), to);
    }

    if (mt == CASTLING) {
        const bool   kingSide = to > from;
        const Square rookFrom = kingSide ? static_cast<Square>(to + 1)
                                         : static_cast<Square>(to - 2);
        const Square rookTo   = kingSide ? static_cast<Square>(to - 1)
                                         : static_cast<Square>(to + 1);
        move_piece(to, from);          // king back
        move_piece(rookTo, rookFrom);  // rook back
    } else {
        move_piece(to, from);          // moving piece back
        if (st.captured != NO_PIECE) {
            const Square capSq = (mt == EN_PASSANT) ? static_cast<Square>(to - pawn_push(us))
                                                    : to;
            put_piece(st.captured, capSq);
        }
    }

    // Restore the saved scalar state (key restored exactly).
    castling_ = st.castling;
    ep_       = st.ep;
    rule50_   = st.rule50;
    key_      = st.key;
}

// ---------------------------------------------------------------------------
// Attack / legality queries
// ---------------------------------------------------------------------------
Bitboard Position::attackers_to(Square s, Bitboard occ) const {
    return (pawn_attacks(BLACK, s) & pieces(WHITE, PAWN))
         | (pawn_attacks(WHITE, s) & pieces(BLACK, PAWN))
         | (knight_attacks(s)      & pieces(KNIGHT))
         | (king_attacks(s)        & pieces(KING))
         | (bishop_attacks(s, occ) & pieces(BISHOP, QUEEN))
         | (rook_attacks(s, occ)   & pieces(ROOK, QUEEN));
}

bool Position::is_square_attacked(Square s, Color by, Bitboard occ) const {
    if (pawn_attacks(~by, s) & pieces(by, PAWN))                  return true;
    if (knight_attacks(s)    & pieces(by, KNIGHT))               return true;
    if (king_attacks(s)      & pieces(by, KING))                 return true;
    if (bishop_attacks(s, occ) & pieces(by, BISHOP, QUEEN))      return true;
    if (rook_attacks(s, occ)   & pieces(by, ROOK, QUEEN))        return true;
    return false;
}

Bitboard Position::checkers() const {
    const Square ksq = king_sq(sideToMove_);
    return attackers_to(ksq, pieces()) & pieces(~sideToMove_);
}

Bitboard Position::pinned(Color c) const {
    const Square   ksq = king_sq(c);
    const Bitboard occ = pieces();
    Bitboard       result = 0;

    // Enemy sliders that would attack the king on an empty board.
    Bitboard snipers =
        (rook_attacks(ksq, 0)   & pieces(~c, ROOK, QUEEN))
      | (bishop_attacks(ksq, 0) & pieces(~c, BISHOP, QUEEN));

    while (snipers) {
        const Square   sniperSq = pop_lsb(snipers);
        const Bitboard between   = between_bb(ksq, sniperSq) & occ;
        // Exactly one piece between king and sniper, and it is ours -> pinned.
        if (between && !more_than_one(between) && (between & pieces(c)))
            result |= between;
    }
    return result;
}

Bitboard Position::attacks_by(Color by, Bitboard occ) const {
    Bitboard attacked = 0;

    attacked |= pawn_attacks_bb(by, pieces(by, PAWN));

    Bitboard knights = pieces(by, KNIGHT);
    while (knights) attacked |= knight_attacks(pop_lsb(knights));

    Bitboard bishops = pieces(by, BISHOP, QUEEN);
    while (bishops) { const Square s = pop_lsb(bishops); attacked |= bishop_attacks(s, occ); }

    Bitboard rooks = pieces(by, ROOK, QUEEN);
    while (rooks) { const Square s = pop_lsb(rooks); attacked |= rook_attacks(s, occ); }

    attacked |= king_attacks(king_sq(by));
    return attacked;
}

// An ep square contributes to the hash key only when the side to move could
// genuinely answer it with a pawn capture. The squares from which a pawn of
// colour `stm` attacks `ep` are exactly pawn_attacks(~stm, ep).
bool Position::ep_capturable(Square ep, Color stm) const {
    if (ep == SQ_NONE) return false;
    return (pawn_attacks(~stm, ep) & pieces(stm, PAWN)) != 0;
}

Key Position::compute_key() const {
    Key k = 0;
    Bitboard occ = pieces();
    while (occ) {
        const Square s = pop_lsb(occ);
        k ^= Zobrist::psq[board_[s]][s];
    }
    if (sideToMove_ == BLACK) k ^= Zobrist::side;
    k ^= Zobrist::castling[castling_];
    if (ep_capturable(ep_, sideToMove_)) k ^= Zobrist::enpassant[file_of(ep_)];
    return k;
}

// ---------------------------------------------------------------------------
// Draw detection: fifty-move rule OR repetition.
// ---------------------------------------------------------------------------
bool Position::is_draw() const {
    if (rule50_ >= 100)
        return true;

    const int n = static_cast<int>(keyHistory_.size());
    // The current position is keyHistory_[n-1]. A repetition can only recur
    // with the same side to move, so scan back in steps of two, and never
    // beyond the irreversible window (rule50_ plies).
    const int end = std::min(rule50_, n - 1);
    for (int i = 4; i <= end; i += 2) {
        if (keyHistory_[n - 1 - i] == key_)
            return true;
    }
    return false;
}
