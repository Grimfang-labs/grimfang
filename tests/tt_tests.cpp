#include <catch2/catch_test_macros.hpp>

#include "move.hpp"
#include "score.hpp"
#include "tt.hpp"
#include "types.hpp"

// ===========================================================================
// tt_tests.cpp - Stage 3 rung 3.1 gate.
//
//   * mate-score round-trip (value_to_tt / value_from_tt) - the #1 TT bug
//   * store/probe correctness (move, bound, depth survive a round trip)
//   * key32 verification rejects same-index different-position collisions
//   * resize / clear / hashfull behave sanely
// ===========================================================================

namespace {

// Recover a (signed) mate distance in *plies* from a root-relative score.
int mate_ply(Value score) {
    if (score >= VALUE_MATE_IN_MAX_PLY)  return VALUE_MATE - score;
    if (score <= -VALUE_MATE_IN_MAX_PLY) return -(VALUE_MATE + score);
    return 0;
}

} // namespace

TEST_CASE("tt: mate-score round-trips regardless of store/probe ply", "[tt][mate]") {
    // A mate-in-N delivered N plies from the *root* is encoded VALUE_MATE - N.
    for (int matePlies = 1; matePlies <= 30; ++matePlies) {
        const Value rootScore = static_cast<Value>(VALUE_MATE - matePlies);

        // Store at ply p1, read back at ply p2 -> the recovered root score must
        // describe the same mate distance for *every* pairing.
        for (int p1 = 0; p1 <= 20; ++p1) {
            const Value stored = value_to_tt(rootScore, p1);
            for (int p2 = 0; p2 <= 20; ++p2) {
                // The mate distance measured from the probing node must match
                // the original mate distance measured from the storing node:
                // (matePlies - p1) is the distance below the storing node.
                const Value recovered = value_from_tt(stored, p2);
                REQUIRE(mate_ply(recovered) == matePlies - p1 + p2);
            }
        }
    }

    // The classic invariant: store and read at the *same* ply is the identity.
    for (int ply = 0; ply <= 40; ++ply) {
        const Value s = static_cast<Value>(VALUE_MATE - 7);
        REQUIRE(value_from_tt(value_to_tt(s, ply), ply) == s);
        const Value sNeg = static_cast<Value>(-(VALUE_MATE - 7));
        REQUIRE(value_from_tt(value_to_tt(sNeg, ply), ply) == sNeg);
    }
}

TEST_CASE("tt: non-mate values are stored verbatim", "[tt]") {
    for (int ply = 0; ply <= 30; ++ply) {
        for (Value v : { Value(0), Value(15), Value(-15), Value(2500), Value(-2500) }) {
            REQUIRE(value_to_tt(v, ply) == v);
            REQUIRE(value_from_tt(v, ply) == v);
        }
    }
}

TEST_CASE("tt: store then probe returns move/bound/depth", "[tt]") {
    TT.resize(1);
    TT.clear();
    TT.new_search();

    const Key  key  = 0x0123456789ABCDEFULL;
    const Move move = Move(E2, E4);

    TT.store(key, move, /*v=*/123, /*eval=*/VALUE_NONE, /*depth=*/9, BOUND_EXACT, /*ply=*/4);

    TTEntry* e = nullptr;
    REQUIRE(TT.probe(key, e));
    REQUIRE(e != nullptr);
    REQUIRE(e->move() == move);
    REQUIRE(e->bound() == BOUND_EXACT);
    REQUIRE(e->depth() == 9);
    REQUIRE(value_from_tt(e->value(), 4) == 123);
}

TEST_CASE("tt: key32 verification rejects same-index collisions", "[tt]") {
    TT.resize(1);
    TT.clear();
    TT.new_search();

    const std::size_t clusters = TT.cluster_count();
    REQUIRE(clusters > 0);

    // Two keys that share the same low bits (same cluster index) but differ in
    // the high 32 bits must NOT alias: the second must not read the first.
    const Key low  = 0x00000000DEADBEEFULL & (static_cast<Key>(clusters) - 1);
    const Key keyA = (static_cast<Key>(0x11111111ULL) << 32) | low;
    const Key keyB = (static_cast<Key>(0x22222222ULL) << 32) | low;

    TT.store(keyA, Move(A2, A3), /*v=*/50, VALUE_NONE, /*depth=*/5, BOUND_EXACT, /*ply=*/0);

    TTEntry* e = nullptr;
    REQUIRE(TT.probe(keyA, e));        // A is present
    REQUIRE_FALSE(TT.probe(keyB, e));  // B (same index, different key32) is not
}

TEST_CASE("tt: probe miss on a cleared table", "[tt]") {
    TT.resize(1);
    TT.clear();
    TT.new_search();

    TTEntry* e = nullptr;
    REQUIRE_FALSE(TT.probe(0xCAFEF00DBAADF00DULL, e));
}

TEST_CASE("tt: resize yields a power-of-two cluster count and clears", "[tt]") {
    TT.resize(64);
    const std::size_t c = TT.cluster_count();
    REQUIRE(c > 0);
    REQUIRE((c & (c - 1)) == 0);   // power of two

    TT.clear();
    REQUIRE(TT.hashfull() == 0);
}

TEST_CASE("tt: hashfull rises as entries are stored", "[tt]") {
    TT.resize(1);
    TT.clear();
    TT.new_search();
    REQUIRE(TT.hashfull() == 0);

    // Fill well beyond the 1000-entry sample window.
    for (Key k = 1; k <= 4000; ++k)
        TT.store(k << 1, Move(B1, C3), /*v=*/0, VALUE_NONE, /*depth=*/3, BOUND_EXACT, /*ply=*/0);

    REQUIRE(TT.hashfull() > 0);
}
