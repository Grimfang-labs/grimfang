#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "history.hpp"
#include "killers.hpp"   // is_quiet_move (shared quiet classifier)
#include "move.hpp"
#include "position.hpp"
#include "types.hpp"
#include "zobrist.hpp"

// ===========================================================================
// history_tests.cpp - Stage 3 rung 3.4 gate.
//
//   * gravity: repeated bonuses converge toward +MAX_HISTORY without ever
//     wrapping the int16; repeated penalties converge toward -MAX_HISTORY;
//     a bonused move outranks an untouched move outranks a penalised move.
//   * targeting: a simulated quiet cutoff bonuses the cutoff move and
//     penalises ONLY the quiets tried before it (captures and later moves
//     are left untouched).
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

} // namespace

TEST_CASE("history: gravity converges without overflowing int16", "[history]") {
    HistoryTable h;
    h.clear();

    const Color c         = WHITE;
    const Move  bonused   = Move(E2, E4);
    const Move  penalised = Move(D2, D4);
    const Move  untouched = Move(G1, F3);

    // Hammer both directions far past what a real search would apply.
    for (int k = 0; k < 5000; ++k) {
        h.update(c, bonused,   HISTORY_BONUS_CAP);
        h.update(c, penalised, -HISTORY_BONUS_CAP);
        // Must never wrap into the wrong sign on the way up/down.
        REQUIRE(h.get(c, bonused)   > 0);
        REQUIRE(h.get(c, penalised) < 0);
    }

    const int hb = h.get(c, bonused);
    const int hp = h.get(c, penalised);

    // Converged near +/-MAX_HISTORY, safely inside the int16 range.
    REQUIRE(hb <= MAX_HISTORY);
    REQUIRE(hb >  MAX_HISTORY - HISTORY_BONUS_CAP);
    REQUIRE(hp >= -MAX_HISTORY);
    REQUIRE(hp <  -(MAX_HISTORY - HISTORY_BONUS_CAP));
    REQUIRE(hb < 32767);
    REQUIRE(hp > -32768);

    // Ordering: bonused > untouched (0) > penalised.
    REQUIRE(h.get(c, untouched) == 0);
    REQUIRE(hb > h.get(c, untouched));
    REQUIRE(h.get(c, untouched) > hp);
}

TEST_CASE("history: a single bonus and its gravity pull-back", "[history]") {
    HistoryTable h;
    h.clear();

    // First application of +delta to a zero entry is exactly +delta.
    h.update(WHITE, Move(B1, C3), 100);
    REQUIRE(h.get(WHITE, Move(B1, C3)) == 100);

    // Near the ceiling the same delta barely moves the entry (gravity).
    HistoryTable h2;
    h2.clear();
    for (int k = 0; k < 2000; ++k)
        h2.update(WHITE, Move(B1, C3), HISTORY_BONUS_CAP);
    const int before = h2.get(WHITE, Move(B1, C3));
    h2.update(WHITE, Move(B1, C3), HISTORY_BONUS_CAP);
    const int after = h2.get(WHITE, Move(B1, C3));
    REQUIRE(after - before < HISTORY_BONUS_CAP / 4);   // strongly damped
}

TEST_CASE("history: cutoff bonuses the mover and penalises only earlier quiets",
          "[history]") {
    // White: Ra1, Ke1.  Black: Ra8, Ke8.
    Position pos;
    pos.set_fen("r3k3/8/8/8/8/8/8/R3K3 w Qq - 0 1");

    HistoryTable h;
    h.clear();

    const Color stm   = pos.side_to_move();     // WHITE
    const int   depth = 6;
    const int   bonus = history_bonus(depth);   // min(36, 1600) = 36
    REQUIRE(bonus == 36);

    // A plausible ordered move list as tried by the search, with the cutoff at
    // index 3. Index 1 is a capture; index 4 comes after the cutoff.
    const Move quietA  = Move(A1, A2);
    const Move capture = Move(A1, A8);           // Rxa8
    const Move quietB  = Move(A1, A3);
    const Move cutoff  = Move(A1, A4);
    const Move quietD  = Move(A1, A5);

    REQUIRE(is_quiet_move(pos, quietA));
    REQUIRE_FALSE(is_quiet_move(pos, capture));
    REQUIRE(is_quiet_move(pos, cutoff));

    const Move tried[] = { quietA, capture, quietB, cutoff, quietD };
    const int  cutoffIdx = 3;

    // Mirror the search's update: reward the cutoff mover, penalise quiets
    // tried before it.
    h.update(stm, tried[cutoffIdx], bonus);
    for (int j = 0; j < cutoffIdx; ++j)
        if (is_quiet_move(pos, tried[j]))
            h.update(stm, tried[j], -bonus);

    REQUIRE(h.get(stm, cutoff)  ==  bonus);   // rewarded
    REQUIRE(h.get(stm, quietA)  == -bonus);   // earlier quiet penalised
    REQUIRE(h.get(stm, quietB)  == -bonus);   // earlier quiet penalised
    REQUIRE(h.get(stm, capture) ==  0);       // capture untouched
    REQUIRE(h.get(stm, quietD)  ==  0);       // later move untouched

    // Resulting ordering among these quiets: cutoff > untouched > penalised.
    REQUIRE(h.get(stm, cutoff) > h.get(stm, quietD));
    REQUIRE(h.get(stm, quietD) > h.get(stm, quietA));
}
