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
// lmr_tests.cpp - Stage 3 rung 3.7 gate.
//
//   * Collapse guard: at depth 2 no node satisfies depth >= LMR_MIN_DEPTH (3),
//     so the move loop must take the identical pre-LMR PVS path (R == 0).
//     Reproducibility at depth 2 is the in-process check; byte-identical
//     output vs the RFP baseline is verified by the handoff script.
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
