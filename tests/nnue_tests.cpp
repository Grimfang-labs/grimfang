#include <array>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "eval.hpp"
#include "move.hpp"
#include "nnue.hpp"
#include "position.hpp"
#include "zobrist.hpp"

namespace {

struct CoreInit {
    CoreInit() {
        BB::init();
        Zobrist::init();
        Attacks::init();
    }
};
const CoreInit g_coreInit;

const std::array<std::pair<const char*, Value>, 10> kVerifierCases {{
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 179 },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", -4 },
    { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 61 },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 70 },
    { "8/8/8/4k3/8/3K4/4P3/8 b - - 0 1", -140 },
    { "rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq c6 0 3", 115 },
    { "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 87 },
    { "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1", 832 },
    { "4k3/8/8/8/8/8/4Q3/4K3 b - - 0 1", -683 },
    { "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", 1312 },
}};

} // namespace

TEST_CASE("nnue: verifier positions match exact quantised evals", "[eval][nnue]") {
    for (const auto& [fen, expected] : kVerifierCases) {
        Position pos;
        pos.set_fen(fen);
        REQUIRE(Eval::evaluate(pos) == expected);
    }
}

TEST_CASE("nnue: incremental accumulators match full refresh on tricky move paths", "[nnue]") {
    Position pos;
    pos.set_fen("r3k2r/ppp2ppp/8/3Pp3/8/8/PPP2PPP/R3K2R w KQkq e6 0 1");

    const std::vector<Move> moves = {
        Move::make<EN_PASSANT>(D5, E6),
        Move::make<CASTLING>(E8, G8),
        Move::make<CASTLING>(E1, C1),
    };

    for (Move move : moves) {
        pos.make_move(move);
        nnue::AccumulatorPair scratch;
        nnue::refresh(scratch, pos);
        REQUIRE(scratch.byPerspective == pos.nnue_accumulators().byPerspective);
    }

    Position promo;
    promo.set_fen("4k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
    promo.make_move(Move::make<PROMOTION>(B7, B8, QUEEN));
    nnue::AccumulatorPair scratch;
    nnue::refresh(scratch, promo);
    REQUIRE(scratch.byPerspective == promo.nnue_accumulators().byPerspective);
}
