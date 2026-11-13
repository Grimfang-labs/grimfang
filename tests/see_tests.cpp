#include <string>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "move.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "see.hpp"
#include "zobrist.hpp"

// ===========================================================================
// see_tests.cpp - static exchange evaluation (SEE).
//
// Each case gives a position, a legal move (UCI) and the exact exchange value
// with P=100 N=320 B=330 R=500 Q=900. see_ge must be true at that threshold
// and false one centipawn above it, which pins the value down exactly.
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

constexpr int P = 100, N = 320, B = 330, R = 500, Q = 900;

Move find_move(const Position& pos, const std::string& uci) {
    MoveList list;
    generate_legal(pos, list);
    for (int i = 0; i < list.count; ++i)
        if (to_uci(list.moves[i]) == uci)
            return list.moves[i];
    return MOVE_NONE;
}

struct SeeCase {
    const char* fen;
    const char* move;
    int         value;   // exact SEE value of the move
};

void check_case(const SeeCase& c) {
    Position pos;
    pos.set_fen(c.fen);
    const Move m = find_move(pos, c.move);
    INFO("fen: " << c.fen << "  move: " << c.move << "  expected: " << c.value);
    REQUIRE(m != MOVE_NONE);
    REQUIRE(see_ge(pos, m, c.value));
    REQUIRE_FALSE(see_ge(pos, m, c.value + 1));
}

} // namespace

TEST_CASE("see: simple captures", "[see]") {
    const SeeCase cases[] = {
        // Undefended pawn: free material.
        { "1k6/8/8/4p3/8/8/8/1K2R3 w - - 0 1", "e1e5", P },
        // Pawn defended by a pawn: RxP PxR.
        { "1k6/5p2/4p3/8/8/8/8/1K2R3 w - - 0 1", "e1e6", P - R },
        // Equal trade: NxN, recaptured by a pawn.
        { "1k6/8/3p4/4n3/8/5N2/8/1K6 w - - 0 1", "f3e5", 0 },
        // Pawn takes a defended knight: still wins N - P.
        { "1k6/8/3p4/4n3/3P4/8/8/1K6 w - - 0 1", "d4e5", N - P },
        // Queen takes a pawn defended by a knight.
        { "1k6/8/2n5/4p3/8/8/8/1K2Q3 w - - 0 1", "e1e5", P - Q },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: recaptures and stopping early", "[see]") {
    const SeeCase cases[] = {
        // RxN, BxR, RxB: white wins N + B - R.
        { "1k6/8/8/2b5/8/4n3/4R3/1K2R3 w - - 0 1", "e2e3", N - R + B },
        // NxP defended by a pawn and attacked twice: NxP PxN, then white stops.
        { "1k6/8/5p2/4p3/8/3N4/8/1K2R3 w - - 0 1", "d3e5", P - N + P },
        // Undefended capturer: BxN QxB nets N - B.
        { "1k6/8/8/3q4/4n3/8/6B1/1K6 w - - 0 1", "g2e4", N - B },
        // With a rook backing e4 black declines QxB (it would lose the queen).
        { "1k6/8/8/3q4/4n3/8/6B1/1K2R3 w - - 0 1", "g2e4", N },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: x-ray attackers behind sliders", "[see]") {
    const SeeCase cases[] = {
        // Doubled rooks vs one defender: RxP RxR RxR wins P (+R-R).
        { "1k2r3/8/8/4p3/8/8/4R3/1K2R3 w - - 0 1", "e2e5", P },
        // Rook vs rook-backed-by-queen defender stack: RxP RxR QxR? Only one
        // white attacker so the exchange stops: P - R.
        { "1k2q3/4r3/8/4p3/8/8/8/1K2R3 w - - 0 1", "e1e5", P - R },
        // Queen behind bishop on the diagonal: BxP PxB QxP wins P - B + P.
        { "1k6/8/5p2/4p3/3B4/2Q5/8/1K6 w - - 0 1", "d4e5", P - B + P },
        // Black x-ray: rook behind rook on the e-file defends twice.
        { "1k2r3/4r3/8/4p3/8/8/4R3/1K2R3 w - - 0 1", "e2e5", P - R },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: king may only capture an undefended piece", "[see]") {
    const SeeCase cases[] = {
        // RxP, black recaptures with the king (white has no second attacker).
        { "8/8/4k3/4p3/8/8/8/1K2R3 w - - 0 1", "e1e5", P - R },
        // Second white attacker: the black king cannot recapture.
        { "8/8/4k3/4p3/8/8/4R3/1K2R3 w - - 0 1", "e2e5", P },
        // A king capturing an undefended piece simply wins it.
        { "k7/8/8/8/4n3/4K3/8/8 w - - 0 1", "e3e4", N },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: en passant", "[see]") {
    const SeeCase cases[] = {
        // Undefended en passant capture.
        { "1k6/8/8/3pP3/8/8/8/1K6 w - d6 0 1", "e5d6", P },
        // En passant into a pawn-defended square: PxP PxP.
        { "1k6/2p5/8/3pP3/8/8/8/1K6 w - d6 0 1", "e5d6", 0 },
        // Removing the captured pawn opens the d-file: the black rook on d8
        // now defends d6, the white rook on d1 backs the exchange up.
        { "1k1r4/8/8/3pP3/8/8/8/1K1R4 w - d6 0 1", "e5d6", P },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: promotions", "[see]") {
    const SeeCase cases[] = {
        // Quiet queen promotion on a safe square.
        { "1k6/4P3/8/8/8/8/8/1K6 w - - 0 1", "e7e8q", Q - P },
        // Quiet queen promotion on a rook-defended square: lose the pawn.
        { "4r1k1/3P4/8/8/8/8/8/1K6 w - - 0 1", "d7d8q", -P },
        // Capture-promotion of an undefended rook.
        { "3r2k1/4P3/8/8/8/8/8/1K6 w - - 0 1", "e7d8q", R + Q - P },
        // Under-promotion to a knight on a safe square.
        { "1k6/4P3/8/8/8/8/8/1K6 w - - 0 1", "e7e8n", N - P },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: non-captures and castling", "[see]") {
    const SeeCase cases[] = {
        // Quiet move to a safe square.
        { "1k6/8/8/8/8/8/8/1K2R3 w - - 0 1", "e1e4", 0 },
        // Quiet move into a pawn attack hangs the rook.
        { "1k6/8/8/3p4/8/8/8/1K2R3 w - - 0 1", "e1e4", -R },
        // Castling is material-neutral.
        { "1k6/8/8/8/8/8/8/4K2R w K - 0 1", "e1g1", 0 },
    };
    for (const auto& c : cases)
        check_case(c);
}

TEST_CASE("see: black to move mirrors white", "[see]") {
    const SeeCase cases[] = {
        { "1k2r3/8/8/8/4P3/3P4/8/1K6 b - - 0 1", "e8e4", P - R },
        { "1k2r3/4r3/8/8/4P3/8/8/1K2R3 b - - 0 1", "e7e4", P },
    };
    for (const auto& c : cases)
        check_case(c);
}
