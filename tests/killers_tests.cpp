#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "killers.hpp"
#include "move.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "score.hpp"
#include "types.hpp"
#include "zobrist.hpp"

// ===========================================================================
// killers_tests.cpp - Stage 3 rung 3.3 gate.
//
//   * KillerTable update: fill slot 0, shift on a new move, no duplication.
//   * Only quiet cutoff moves are recorded (captures/promotions excluded).
//   * Ordering: an absent/illegal killer contributes no bonus (silently
//     skipped); a legal killer is prioritised between captures and quiets.
// ===========================================================================

namespace {

struct CoreInit {
    CoreInit() {
        BB::init();
        Zobrist::init();
        Attacks::init();
    }
};
const CoreInit g_coreInit;

bool list_contains(const MoveList& list, Move m) {
    for (int i = 0; i < list.count; ++i)
        if (list.moves[i] == m) return true;
    return false;
}

} // namespace

TEST_CASE("killers: update fills, shifts, and de-dups", "[killers]") {
    KillerTable kt;
    kt.clear();

    const int  ply = 5;
    const Move a   = Move(E2, E4);
    const Move b   = Move(D2, D4);

    REQUIRE(kt.primary(ply)   == MOVE_NONE);
    REQUIRE(kt.secondary(ply) == MOVE_NONE);

    kt.update(ply, a);                       // first cutoff fills slot 0
    REQUIRE(kt.primary(ply)   == a);
    REQUIRE(kt.secondary(ply) == MOVE_NONE);

    kt.update(ply, b);                        // different move shifts
    REQUIRE(kt.primary(ply)   == b);
    REQUIRE(kt.secondary(ply) == a);

    kt.update(ply, b);                        // same as slot 0 -> no duplicate
    REQUIRE(kt.primary(ply)   == b);
    REQUIRE(kt.secondary(ply) == a);

    // Killers are independent across plies.
    REQUIRE(kt.primary(ply + 1)   == MOVE_NONE);
    REQUIRE(kt.secondary(ply + 1) == MOVE_NONE);
}

TEST_CASE("killers: quiet classification matches the search guard", "[killers]") {
    // White: Ra1, Ke1.  Black: Ra8, Ke8.
    Position pos;
    pos.set_fen("r3k3/8/8/8/8/8/8/R3K3 w Qq - 0 1");

    REQUIRE(is_quiet_move(pos, Move(A1, A4)));                         // rook to empty
    REQUIRE_FALSE(is_quiet_move(pos, Move(A1, A8)));                   // rook takes rook
    REQUIRE_FALSE(is_quiet_move(pos, Move::make<PROMOTION>(A7, A8)));  // promotion
    REQUIRE_FALSE(is_quiet_move(pos, Move::make<EN_PASSANT>(B5, A6))); // en passant
}

TEST_CASE("killers: only quiet cutoff moves are recorded", "[killers]") {
    Position pos;
    pos.set_fen("r3k3/8/8/8/8/8/8/R3K3 w Qq - 0 1");

    KillerTable kt;
    kt.clear();
    const int ply = 3;

    const Move capture = Move(A1, A8);   // legal capture in this position
    const Move quiet   = Move(A1, A4);   // legal quiet in this position

    // Mirror the search's cutoff guard exactly.
    if (is_quiet_move(pos, capture)) kt.update(ply, capture);
    REQUIRE(kt.primary(ply) == MOVE_NONE);     // a capture is never a killer

    if (is_quiet_move(pos, quiet)) kt.update(ply, quiet);
    REQUIRE(kt.primary(ply) == quiet);         // a quiet refutation is stored
}

TEST_CASE("killers: absent/illegal killer contributes no ordering bonus", "[killers]") {
    Position pos;
    pos.set_fen("r3k3/8/8/8/8/8/8/R3K3 w Qq - 0 1");

    MoveList legal;
    generate_legal(pos, legal);

    // A killer carried over from a sibling branch that is illegal here (no
    // friendly piece on h1, so it is never generated).
    const Move illegalKiller = Move(H1, H8);
    REQUIRE_FALSE(list_contains(legal, illegalKiller));

    // Ordering only scores moves that exist in the legal list, so an absent
    // killer can never be selected or injected: every legal move scores 0
    // against it.
    for (int i = 0; i < legal.count; ++i)
        REQUIRE(killer_bonus(legal.moves[i], illegalKiller, MOVE_NONE) == 0);

    // A killer that IS legal here is prioritised among quiets.
    const Move legalKiller1 = Move(A1, A4);
    const Move legalKiller2 = Move(A1, A3);
    REQUIRE(list_contains(legal, legalKiller1));
    REQUIRE(list_contains(legal, legalKiller2));
    REQUIRE(killer_bonus(legalKiller1, legalKiller1, legalKiller2) == KILLER1_BONUS);
    REQUIRE(killer_bonus(legalKiller2, legalKiller1, legalKiller2) == KILLER2_BONUS);
    REQUIRE(killer_bonus(Move(A1, A2), legalKiller1, legalKiller2) == 0);
}

TEST_CASE("killers: bonus sits between captures/promotions and quiets", "[killers]") {
    REQUIRE(KILLER1_BONUS > KILLER2_BONUS);
    REQUIRE(KILLER2_BONUS > 0);
    // Below the promotion floor (PROMOTION_BONUS = 900'000 in search.cpp) so
    // captures and promotions still order ahead of killers.
    REQUIRE(KILLER1_BONUS < 900'000);
}
