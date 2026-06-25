#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "zobrist.hpp"

// ===========================================================================
// perft_tests.cpp - Stage 1 test gate.
//
// Sub-step 1: a smoke test proving the engine tables initialise. Real perft
// gates against the reference node counts are added in sub-step 3.
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

TEST_CASE("core initialises", "[smoke]") {
    REQUIRE(popcount(0xFFULL) == 8);
    REQUIRE(knight_attacks(A1) != 0);
}
