#include <cstdint>
#include <utility>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "move.hpp"
#include "movegen.hpp"
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

// ---------------------------------------------------------------------------
// Position::gives_check must agree with make_move + checkers() for every
// legal move of every position in a perft-style tree walk (covers direct,
// discovered, en passant, castling and promotion checks).
// ---------------------------------------------------------------------------
namespace {

std::uint64_t check_gives_check(Position& pos, int depth, std::uint64_t& checks) {
    MoveList list;
    generate_legal(pos, list);
    std::uint64_t n = 0;
    for (const Move m : list) {
        const bool predicted = pos.gives_check(m);
        pos.make_move(m);
        const bool actual = pos.checkers() != 0;
        // The cached checkers must equal a recomputation.
        const Color stm = pos.side_to_move();
        REQUIRE(pos.checkers()
                == (pos.attackers_to(pos.king_sq(stm)) & pos.pieces(~stm)));
        if (predicted != actual) {
            pos.unmake_move(m);
            FAIL("gives_check mismatch: " << pos.fen() << " move " << to_uci(m)
                 << " predicted " << predicted << " actual " << actual);
        }
        checks += actual;
        ++n;
        if (depth > 1)
            n += check_gives_check(pos, depth - 1, checks);
        pos.unmake_move(m);
    }
    return n;
}

} // namespace

TEST_CASE("gives_check matches make_move + checkers", "[movegen][givescheck]") {
    const std::vector<std::pair<std::string, int>> cases = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4 },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3 },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3 },
        { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3 },
        // Castling with check by the rook (white O-O-O gives check on d-file).
        { "3k4/8/8/8/8/8/8/R3K2R w KQ - 0 1", 3 },
        { "5k2/8/8/8/8/8/8/R3K2R w KQ - 0 1", 3 },
        // En passant discovered checks (rank and diagonal) and ep direct check.
        { "8/8/8/K2Pp2k/8/8/8/8 w - e6 0 1", 4 },
        { "8/8/8/1k1Pp2R/8/8/8/4K3 w - e6 0 1", 3 },
        { "8/8/3k4/4pP2/8/8/8/B3K3 w - e6 0 1", 3 },
        // Promotions with direct and discovered checks.
        { "3k4/1P6/8/8/8/8/6p1/4K2R b K - 0 1", 3 },
        { "1k6/3P4/8/8/8/8/3R4/4K3 w - - 0 1", 3 },
        { "r3k3/1P6/8/8/8/8/8/R3K3 w Qq - 0 1", 3 },
    };
    std::uint64_t total = 0, checks = 0;
    for (const auto& [fen, depth] : cases) {
        Position pos;
        pos.set_fen(fen);
        total += check_gives_check(pos, depth, checks);
    }
    REQUIRE(total > 100000);
    REQUIRE(checks > 1000);
}

// ---------------------------------------------------------------------------
// Position::is_legal(Move) must accept exactly the moves generate_legal
// produces: every 16-bit move value is tried at every node of short tree
// walks (TT moves can be arbitrary values from another position).
// ---------------------------------------------------------------------------
namespace {

void check_is_legal(Position& pos, int depth, std::uint64_t& nodes) {
    MoveList list;
    generate_legal(pos, list);
    std::vector<bool> isGen(65536, false);
    for (const Move m : list)
        isGen[m.raw()] = true;
    for (int raw = 0; raw < 65536; ++raw) {
        const Move m(static_cast<std::uint16_t>(raw));
        if (pos.is_legal(m) != isGen[raw])
            FAIL("is_legal mismatch: " << pos.fen() << " move " << to_uci(m)
                 << " raw " << raw << " generated " << isGen[raw]);
    }
    ++nodes;
    if (depth > 1)
        for (const Move m : list) {
            pos.make_move(m);
            check_is_legal(pos, depth - 1, nodes);
            pos.unmake_move(m);
        }
}

} // namespace

TEST_CASE("is_legal matches generate_legal for every move value", "[movegen][islegal]") {
    const std::vector<std::pair<std::string, int>> cases = {
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3 },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4 },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 2 },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2 },
        { "8/8/8/1k1Pp2R/8/8/8/4K3 w - e6 0 1", 2 },
        { "8/8/3k4/4pP2/8/8/8/B3K3 w - e6 0 1", 2 },
        { "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", 2 },
        { "4k3/8/8/8/1b6/8/8/R3K2R w KQ - 0 1", 2 },
    };
    std::uint64_t nodes = 0;
    for (const auto& [fen, depth] : cases) {
        Position pos;
        pos.set_fen(fen);
        check_is_legal(pos, depth, nodes);
    }
    REQUIRE(nodes > 2000);
}
