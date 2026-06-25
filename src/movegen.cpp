#include "movegen.hpp"

#include "attacks.hpp"
#include "bitboard.hpp"
#include "position.hpp"

// ===========================================================================
// movegen.cpp - fully-legal, mask-based move generation.
//
// The generator never makes/unmakes a move to test legality; it instead uses
// checkers, pins and an enemy attack map to emit only legal moves.
//
// Three classic perft-failure sources are handled explicitly (see comments):
//   1. King stepping along a slider check ray.
//   2. Castling through/into check.
//   3. En passant discovered check.
// ===========================================================================

namespace {

constexpr Bitboard FULL_BB = ~0ULL;

// Emit a pawn move, expanding to four promotions on the last rank.
inline void add_pawn_move(MoveList& list, Color us, Square from, Square to) {
    if (relative_rank(us, to) == RANK_8) {
        list.add(Move::make<PROMOTION>(from, to, QUEEN));
        list.add(Move::make<PROMOTION>(from, to, ROOK));
        list.add(Move::make<PROMOTION>(from, to, BISHOP));
        list.add(Move::make<PROMOTION>(from, to, KNIGHT));
    } else {
        list.add(Move(from, to));
    }
}

// En passant legality: an ep capture removes BOTH the moving pawn (from `s`)
// and the captured pawn (behind `ep`). Simulate the resulting occupancy and
// confirm the king is not attacked by ANY enemy piece. This single test
// captures every case: horizontal discovered check by a rook/queen, diagonal
// discovered check from the vacated `s`, and resolution of an existing check
// (the captured pawn being the checker is removed; a knight/pawn checker that
// is not removed still attacks and correctly forbids the move).
bool ep_is_legal(const Position& pos, Color us, Square ksq, Square from, Square ep) {
    const Color    them  = ~us;
    const Square   capSq = ep - pawn_push(us);
    const Bitboard occAfter =
        (pos.pieces() ^ square_bb(from) ^ square_bb(capSq)) | square_bb(ep);

    if (rook_attacks(ksq, occAfter)   & pos.pieces(them, ROOK, QUEEN))   return false;
    if (bishop_attacks(ksq, occAfter) & pos.pieces(them, BISHOP, QUEEN)) return false;
    if (knight_attacks(ksq)           & pos.pieces(them, KNIGHT))        return false;

    // Remaining enemy pawns (the captured one removed) attacking the king.
    const Bitboard enemyPawns = pos.pieces(them, PAWN) ^ square_bb(capSq);
    if (pawn_attacks(us, ksq) & enemyPawns) return false;

    return true;
}

} // namespace

void generate_legal(const Position& pos, MoveList& list) {
    list.count = 0;

    const Color    us   = pos.side_to_move();
    const Color    them = ~us;
    const Square   ksq  = pos.king_sq(us);
    const Bitboard occ  = pos.pieces();
    const Bitboard usBB = pos.pieces(us);
    const Bitboard them_bb = pos.pieces(them);

    const Bitboard checkers = pos.checkers();
    const Bitboard pinned   = pos.pinned(us);

    // Enemy attack map computed with our king removed from the occupancy, so a
    // slider "sees through" the king square. (Fixes: king stepping along the
    // check ray, and king capturing a piece that is defended along that ray.)
    const Bitboard occNoKing   = occ ^ square_bb(ksq);
    const Bitboard enemyAttacks = pos.attacks_by(them, occNoKing);

    // ---- King moves (always legal to generate) --------------------------
    {
        Bitboard b = king_attacks(ksq) & ~usBB & ~enemyAttacks;
        while (b) list.add(Move(ksq, pop_lsb(b)));
    }

    // ---- Double check: only the king may move ---------------------------
    if (more_than_one(checkers))
        return;

    // ---- Target mask for non-king pieces --------------------------------
    // No check  -> any square not occupied by a friendly piece.
    // One check -> capture the checker or (slider only) block between.
    Bitboard checkMask;
    if (checkers) {
        const Square checkerSq = lsb(checkers);
        checkMask = checkers | between_bb(ksq, checkerSq);
    } else {
        checkMask = FULL_BB;
    }
    const Bitboard targets = checkMask & ~usBB;

    // ---- Knights (a pinned knight can never move) -----------------------
    {
        Bitboard knights = pos.pieces(us, KNIGHT) & ~pinned;
        while (knights) {
            const Square s = pop_lsb(knights);
            Bitboard b = knight_attacks(s) & targets;
            while (b) list.add(Move(s, pop_lsb(b)));
        }
    }

    // ---- Bishops + queens (diagonal component) --------------------------
    {
        Bitboard movers = pos.pieces(us, BISHOP, QUEEN);
        while (movers) {
            const Square s = pop_lsb(movers);
            Bitboard b = bishop_attacks(s, occ) & targets;
            if (pinned & square_bb(s)) b &= line_bb(ksq, s);   // stay on pin ray
            while (b) list.add(Move(s, pop_lsb(b)));
        }
    }

    // ---- Rooks + queens (orthogonal component) --------------------------
    {
        Bitboard movers = pos.pieces(us, ROOK, QUEEN);
        while (movers) {
            const Square s = pop_lsb(movers);
            Bitboard b = rook_attacks(s, occ) & targets;
            if (pinned & square_bb(s)) b &= line_bb(ksq, s);   // stay on pin ray
            while (b) list.add(Move(s, pop_lsb(b)));
        }
    }

    // ---- Pawns ----------------------------------------------------------
    {
        const Direction up    = pawn_push(us);
        const Square     epSq = pos.ep_square();
        Bitboard         pawns = pos.pieces(us, PAWN);

        while (pawns) {
            const Square   s        = pop_lsb(pawns);
            const bool     isPinned = (pinned & square_bb(s)) != 0;
            const Bitboard rayMask  = isPinned ? line_bb(ksq, s) : FULL_BB;

            // Pushes
            const Square push1 = s + up;
            if (pos.empty(push1)) {
                if (square_bb(push1) & targets & rayMask)
                    add_pawn_move(list, us, s, push1);

                // Double push from the relative second rank.
                if (relative_rank(us, s) == RANK_2) {
                    const Square push2 = push1 + up;
                    if (pos.empty(push2) && (square_bb(push2) & targets & rayMask))
                        list.add(Move(s, push2));
                }
            }

            // Captures (incl. promotions)
            Bitboard caps = pawn_attacks(us, s) & them_bb & targets & rayMask;
            while (caps)
                add_pawn_move(list, us, s, pop_lsb(caps));

            // En passant (special legality test; not masked by `targets`).
            if (epSq != SQ_NONE && (pawn_attacks(us, s) & square_bb(epSq))) {
                if (ep_is_legal(pos, us, ksq, s, epSq))
                    list.add(Move::make<EN_PASSANT>(s, epSq));
            }
        }
    }

    // ---- Castling (standard chess only; never out of check) -------------
    if (!checkers) {
        auto path_clear_and_safe = [&](Square s1, Square s2) {
            // Transit squares must be empty AND not attacked by the enemy.
            // Tested with the real occupancy (king still home).
            return pos.empty(s1) && pos.empty(s2)
                && !pos.is_square_attacked(s1, them, occ)
                && !pos.is_square_attacked(s2, them, occ);
        };

        if (us == WHITE) {
            if (pos.can_castle(WHITE_OO) && path_clear_and_safe(F1, G1))
                list.add(Move::make<CASTLING>(E1, G1));
            if (pos.can_castle(WHITE_OOO) && pos.empty(B1) && path_clear_and_safe(D1, C1))
                list.add(Move::make<CASTLING>(E1, C1));
        } else {
            if (pos.can_castle(BLACK_OO) && path_clear_and_safe(F8, G8))
                list.add(Move::make<CASTLING>(E8, G8));
            if (pos.can_castle(BLACK_OOO) && pos.empty(B8) && path_clear_and_safe(D8, C8))
                list.add(Move::make<CASTLING>(E8, C8));
        }
    }
}
