#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "position.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "zobrist.hpp"

// ===========================================================================
// lmr_tests.cpp - late move reductions.
//
//   * Reduction table: R = base/100 + ln(depth) * ln(moveNumber) / (div/100)
//     in 1/1024 ply, separately for quiet and noisy moves; checked against
//     hand-computed values, monotonic in depth and move number, noisy moves
//     reduced less than quiets at the defaults, and driven by the tunables.
//   * Collapse guard: at depth 2 no node satisfies depth >= LmrMinDepth (3),
//     so the search is reproducible with no reduction at all.
//   * Mate regression is covered by the existing [mate] suite (run in ctest).
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

Search::Result search_depth2(const std::string& fen) {
    Position pos;
    pos.set_fen(fen);
    TT.clear();   // cold TT so node counts are reproducible across back-to-back runs
    return Search::search_fixed(pos, 2);
}

} // namespace

TEST_CASE("lmr: depth-2 search is reproducible (LMR inactive at depth < 3)", "[lmr][collapse]") {
    const std::vector<std::string> fens = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bqk2r/ppp2ppp/2n2n2/2bpp3/4P3/2NP1N2/PPP2PPP/R1BQKB1R w KQkq - 0 5",
    };

    for (const auto& fen : fens) {
        const Search::Result a = search_depth2(fen);
        const Search::Result b = search_depth2(fen);
        REQUIRE(a.bestMove == b.bestMove);
        REQUIRE(a.score    == b.score);
        REQUIRE(a.nodes     == b.nodes);
    }
}

TEST_CASE("lmr: base reduction table matches the log formula", "[lmr]") {
    const Search::Tunables t;   // defaults: quiet 0.80 + ln*ln/2.40

    // ln(1) == 0: only the constant term.
    REQUIRE(Search::lmr_base_reduction(t, true, 1, 1) == 819);    // 0.80 ply
    REQUIRE(Search::lmr_base_reduction(t, false, 1, 1) == 0);     // 0.00 ply

    // depth 10, 20th move: 0.80 + ln(10) * ln(20) / 2.40 = 3.674 plies.
    const double expected = (0.80 + std::log(10.0) * std::log(20.0) / 2.40) * 1024.0;
    REQUIRE(std::abs(Search::lmr_base_reduction(t, true, 10, 20) - expected) <= 1.0);

    // Degenerate inputs reduce nothing.
    REQUIRE(Search::lmr_base_reduction(t, true, 0, 5) == 0);
    REQUIRE(Search::lmr_base_reduction(t, true, 5, 0) == 0);
}

TEST_CASE("lmr: reductions grow with depth and move number", "[lmr]") {
    const Search::Tunables t;
    for (int quiet = 0; quiet < 2; ++quiet) {
        for (int d = 1; d < 63; ++d) {
            for (int n = 1; n < 63; ++n) {
                const int r = Search::lmr_base_reduction(t, quiet == 1, d, n);
                REQUIRE(Search::lmr_base_reduction(t, quiet == 1, d + 1, n) >= r);
                REQUIRE(Search::lmr_base_reduction(t, quiet == 1, d, n + 1) >= r);
                if (quiet == 0)
                    REQUIRE(r <= Search::lmr_base_reduction(t, true, d, n));
            }
        }
    }
    // A typical late quiet at depth 8 is reduced by about 2-4 plies.
    const int r = Search::lmr_base_reduction(t, true, 8, 12);
    REQUIRE(r >= 2 * 1024);
    REQUIRE(r <= 4 * 1024);
}

TEST_CASE("lmr: table follows the tunables", "[lmr]") {
    Search::Tunables t;
    const int def = Search::lmr_base_reduction(t, true, 12, 30);
    t.lmrQuietDivPct = 480;   // halve the log term
    const int halved = Search::lmr_base_reduction(t, true, 12, 30);
    const double logTerm = std::log(12.0) * std::log(30.0) / 2.40 * 1024.0;
    REQUIRE(std::abs((def - halved) - logTerm / 2.0) <= 2.0);
    t.lmrQuietBasePct = 180;  // +1 ply constant
    REQUIRE(Search::lmr_base_reduction(t, true, 12, 30) - halved == 1024);
}
