#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "datagen.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "zobrist.hpp"

// ===========================================================================
// datagen_tests.cpp - NNUE datagen infrastructure gate.
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

TEST_CASE("datagen: record round-trip FEN reconstruction", "[datagen]") {
    const std::vector<std::string> fens = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "rnbqkbnr/ppp1pppp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    };

    for (const auto& fen : fens) {
        Position pos;
        pos.set_fen(fen);

        const DatagenRecord rec = encode_record(pos, /*score=*/42);
        const std::string rebuilt = record_to_fen(rec);

        Position pos2;
        pos2.set_fen(rebuilt);
        REQUIRE(pos2.fen() == rebuilt);
        REQUIRE(rebuilt == pos.fen());
    }
}

TEST_CASE("datagen: STM-relative result backfill for a white win", "[datagen]") {
    DatagenRecord whiteRec{};
    whiteRec.stm = WHITE;
    DatagenRecord blackRec{};
    blackRec.stm = BLACK;

    apply_result(whiteRec, WHITE, DatagenOutcome::WhiteWin);
    apply_result(blackRec, BLACK, DatagenOutcome::WhiteWin);

    REQUIRE(whiteRec.result == 2);
    REQUIRE(blackRec.result == 0);

    apply_result(whiteRec, WHITE, DatagenOutcome::Draw);
    apply_result(blackRec, BLACK, DatagenOutcome::Draw);
    REQUIRE(whiteRec.result == 1);
    REQUIRE(blackRec.result == 1);
}

TEST_CASE("datagen: in-check positions are not recorded", "[datagen]") {
    Position pos;
    pos.set_fen("4k3/8/8/8/8/8/4r3/4K3 w - - 0 1");
    REQUIRE(pos.checkers() != 0);
    REQUIRE_FALSE(should_record_position(pos, /*score=*/0));
}

TEST_CASE("datagen: blowout scores are not recorded", "[datagen]") {
    Position pos = Position::startpos();
    REQUIRE(should_record_position(pos, 100));
    REQUIRE_FALSE(should_record_position(pos, 3001));
    REQUIRE_FALSE(should_record_position(pos, -3001));
}

TEST_CASE("datagen: node limit stops near the requested cap", "[datagen][nodes]") {
    Position pos = Position::startpos();
    TT.clear();

    constexpr std::uint64_t kCap = 5000;
    const Search::Result r = Search::search_nodes(pos, kCap);

    REQUIRE(r.nodes >= kCap);
    // Polled every POLL_INTERVAL (2048) nodes in should_stop().
    REQUIRE(r.nodes <= kCap + 2048);
}

TEST_CASE("datagen: sizeof record matches on-disk contract", "[datagen]") {
    REQUIRE(sizeof(DatagenRecord) == 74);
}
