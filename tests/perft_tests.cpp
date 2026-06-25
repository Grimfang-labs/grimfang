#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "move.hpp"
#include "position.hpp"
#include "zobrist.hpp"

// ===========================================================================
// perft_tests.cpp - Stage 1 test gate.
//
// Sub-step 2: FEN round-trip and make/unmake exactness.
// (Perft node-count gates are added in sub-step 3.)
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

const std::vector<std::string> kStandardFens = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
};

// Make `m`, assert exact restoration on unmake.
void check_reversible(const std::string& fen, Move m) {
    Position pos;
    pos.set_fen(fen);
    const std::string fenBefore = pos.fen();
    const Key         keyBefore = pos.key();

    pos.make_move(m);
    REQUIRE(pos.key() == pos.compute_key());   // incremental == from-scratch

    pos.unmake_move(m);
    REQUIRE(pos.fen() == fenBefore);
    REQUIRE(pos.key() == keyBefore);
    REQUIRE(pos.key() == pos.compute_key());
}

} // namespace

TEST_CASE("core initialises", "[smoke]") {
    REQUIRE(popcount(0xFFULL) == 8);
    REQUIRE(knight_attacks(A1) != 0);
}

TEST_CASE("geometry tables", "[bitboard]") {
    REQUIRE(between_bb(A1, A4) == (square_bb(A2) | square_bb(A3)));
    REQUIRE(between_bb(A1, H8) == (square_bb(B2) | square_bb(C3) | square_bb(D4)
                                 | square_bb(E5) | square_bb(F6) | square_bb(G7)));
    REQUIRE(between_bb(A1, B3) == 0);                 // not aligned
    REQUIRE((line_bb(A1, A8) & rank_bb(RANK_1)) == square_bb(A1));
    REQUIRE(line_bb(A1, H8) == 0x8040201008040201ULL);
    REQUIRE(aligned(A1, D4, H8));
    REQUIRE_FALSE(aligned(A1, D4, H7));
}

TEST_CASE("FEN round-trips", "[fen]") {
    for (const auto& fen : kStandardFens) {
        Position pos;
        pos.set_fen(fen);
        REQUIRE(pos.fen() == fen);
        REQUIRE(pos.key() == pos.compute_key());
    }
}

TEST_CASE("startpos make e2e4 yields expected FEN", "[make]") {
    Position pos = Position::startpos();
    pos.make_move(Move(E2, E4));
    REQUIRE(pos.fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    pos.unmake_move(Move(E2, E4));
    REQUIRE(pos.fen() == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

TEST_CASE("make/unmake is exact across move types", "[make][unmake]") {
    // Quiet, double push, capture.
    check_reversible("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", Move(G1, F3));
    check_reversible("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", Move(E2, E4));
    check_reversible("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                     Move(E5, G6));   // knight captures pawn

    // Castling both sides (Kiwipete has all rights).
    check_reversible("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                     Move::make<CASTLING>(E1, G1));
    check_reversible("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                     Move::make<CASTLING>(E1, C1));

    // En passant.
    check_reversible("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 1",
                     Move::make<EN_PASSANT>(E5, F6));

    // Promotion (with and without capture).
    check_reversible("4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
                     Move::make<PROMOTION>(A7, A8, QUEEN));
    check_reversible("1n2k3/P7/8/8/8/8/8/4K3 w - - 0 1",
                     Move::make<PROMOTION>(A7, B8, KNIGHT));   // capture-promote
}
