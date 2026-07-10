#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "eval.hpp"
#include "move.hpp"
#include "movegen.hpp"
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

// Assert the SIMD eval/refresh are bit-identical to the scalar reference for a
// single position, then recurse a bounded, deterministic legal-move walk so a
// few hundred distinct positions are covered from each seed FEN.
void check_simd_matches_scalar(Position& pos, int depth, std::uint64_t& checked) {
    // 1. SIMD refresh must equal the scalar refresh (same integer add/sub math).
    nnue::AccumulatorPair simdAcc;
    nnue::AccumulatorPair scalarAcc;
    nnue::refresh(simdAcc, pos);
    nnue::refresh_scalar(scalarAcc, pos);
    REQUIRE(simdAcc.byPerspective == scalarAcc.byPerspective);

    // 2. SIMD dot product must equal the scalar dot product -> equal Value.
    REQUIRE(nnue::evaluate(pos) == nnue::evaluate_scalar(pos));
    ++checked;

    if (depth == 0)
        return;

    MoveList moves;
    generate_legal(pos, moves);
    for (Move m : moves) {
        pos.make_move(m);
        check_simd_matches_scalar(pos, depth - 1, checked);
        pos.unmake_move(m);
    }
}

const std::array<const char*, 5> kWalkSeeds {{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",          // startpos
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",                        // position 3
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", // position 4
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",        // position 5
}};

} // namespace

TEST_CASE("nnue: AVX2 eval and accumulator are bit-identical to scalar", "[nnue][simd]") {
    // The 10 cross-verification FENs...
    for (const auto& [fen, expected] : kVerifierCases) {
        Position pos;
        pos.set_fen(fen);
        std::uint64_t checked = 0;
        check_simd_matches_scalar(pos, 0, checked);
        (void)expected;
    }

    // ...plus a few hundred positions per seed from a bounded legal-move walk.
    std::uint64_t checked = 0;
    for (const char* fen : kWalkSeeds) {
        Position pos;
        pos.set_fen(fen);
        check_simd_matches_scalar(pos, 3, checked);
    }
    REQUIRE(checked > 500);
}

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
