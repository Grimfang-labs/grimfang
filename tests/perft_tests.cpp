#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "move.hpp"
#include "perft.hpp"
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

TEST_CASE("magic sliders equal the slow reference", "[attacks][magic]") {
    Key seed = 0x1234567ULL;
    auto rng = [&seed]() {
        seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
        return seed * 0x2545F4914F6CDD1DULL;
    };
    for (int sq = A1; sq <= H8; ++sq) {
        const Square s = static_cast<Square>(sq);
        for (int t = 0; t < 64; ++t) {
            const Bitboard occ = rng() & rng();   // sparse-ish occupancy
            REQUIRE(bishop_attacks(s, occ) == bishop_attacks_slow(s, occ));
            REQUIRE(rook_attacks(s, occ)   == rook_attacks_slow(s, occ));
            REQUIRE(queen_attacks(s, occ)  ==
                    (bishop_attacks_slow(s, occ) | rook_attacks_slow(s, occ)));
        }
    }
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

// ---------------------------------------------------------------------------
// Perft node-count gates (the Stage 1 acceptance criteria).
// ---------------------------------------------------------------------------
namespace {
std::uint64_t perft_of(const std::string& fen, int depth) {
    Position pos;
    pos.set_fen(fen);
    return perft(pos, depth, /*bulk=*/false);
}
} // namespace

TEST_CASE("perft startpos", "[perft]") {
    const std::string f = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    REQUIRE(perft_of(f, 1) == 20);
    REQUIRE(perft_of(f, 2) == 400);
    REQUIRE(perft_of(f, 3) == 8902);
    REQUIRE(perft_of(f, 4) == 197281);
    REQUIRE(perft_of(f, 5) == 4865609);
}

TEST_CASE("perft kiwipete", "[perft]") {
    const std::string f = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    REQUIRE(perft_of(f, 1) == 48);
    REQUIRE(perft_of(f, 2) == 2039);
    REQUIRE(perft_of(f, 3) == 97862);
    REQUIRE(perft_of(f, 4) == 4085603);
}

TEST_CASE("perft position 3", "[perft]") {
    const std::string f = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1";
    REQUIRE(perft_of(f, 5) == 674624);
    REQUIRE(perft_of(f, 6) == 11030083);
}

TEST_CASE("perft position 4", "[perft]") {
    const std::string f = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1";
    REQUIRE(perft_of(f, 4) == 422333);
    REQUIRE(perft_of(f, 5) == 15833292);
}

TEST_CASE("perft position 5", "[perft]") {
    const std::string f = "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8";
    REQUIRE(perft_of(f, 3) == 62379);
    REQUIRE(perft_of(f, 4) == 2103487);
}

TEST_CASE("perft position 6", "[perft]") {
    const std::string f = "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10";
    REQUIRE(perft_of(f, 4) == 3894594);
    REQUIRE(perft_of(f, 5) == 164075551);
}

// Deep counts gated behind [slow] (run manually, not in CI).
TEST_CASE("perft deep", "[perft][slow]") {
    REQUIRE(perft_of("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6) == 119060324);
    REQUIRE(perft_of("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 5)
            == 193690690);
    REQUIRE(perft_of("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 7) == 178633661);
    REQUIRE(perft_of("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 6)
            == 706045033);
}
