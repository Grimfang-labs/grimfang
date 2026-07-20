#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "eval.hpp"
#include "move.hpp"
#include "position.hpp"
#include "zobrist.hpp"

// ===========================================================================
// eval_tests.cpp - Stage 2 sub-step 1 gate.
//
//   * colour-mirror symmetry (validates the side-relative sign + table mapping)
//   * material sanity (up a queen scores strongly for the queen's owner)
//   * Zobrist en-passant carry-forward fix (key equality across move orders /
//     phantom ep squares, key difference for a genuinely capturable ep).
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

// Vertically mirror the board and swap colours + side to move. The resulting
// position is the exact colour reflection, so a correct side-relative eval must
// return the identical score. Castling/ep are dropped (eval ignores them).
std::string mirror_fen(const std::string& fen) {
    std::istringstream ss(fen);
    std::string placement, stm;
    ss >> placement >> stm;

    std::vector<std::string> ranks;
    std::string rank;
    for (char c : placement) {
        if (c == '/') { ranks.push_back(rank); rank.clear(); }
        else          { rank += c; }
    }
    ranks.push_back(rank);

    std::string out;
    for (auto it = ranks.rbegin(); it != ranks.rend(); ++it) {
        for (char c : *it) {
            if (std::isalpha(static_cast<unsigned char>(c)))
                c = std::islower(static_cast<unsigned char>(c))
                        ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                        : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out += c;
        }
        if (it + 1 != ranks.rend()) out += '/';
    }

    const std::string newStm = (stm == "w") ? "b" : "w";
    return out + ' ' + newStm + " - - 0 1";
}

Value eval_of(const std::string& fen) {
    Position pos;
    pos.set_fen(fen);
    return Eval::evaluate(pos);
}

} // namespace

TEST_CASE("eval: startpos matches the embedded net", "[eval]") {
    REQUIRE(eval_of("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == 164);
}

TEST_CASE("eval: colour-mirror symmetry", "[eval]") {
    const std::vector<std::string> fens = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "8/8/8/4k3/8/3K4/4P3/8 w - - 0 1",
    };
    for (const auto& fen : fens) {
        const Value a = eval_of(fen);
        const Value b = eval_of(mirror_fen(fen));
        REQUIRE(a == b);
    }
}

TEST_CASE("eval: up a clean queen scores strongly", "[eval]") {
    // White has an extra queen; positive for the side to move when it's White,
    // and the same magnitude negative when it is Black to move (side-relative).
    const std::string whiteUp = "4k3/8/8/8/8/8/8/3QK3 w - - 0 1";
    const std::string whiteUpBlackMoves = "4k3/8/8/8/8/8/8/3QK3 b - - 0 1";

    REQUIRE(eval_of(whiteUp) > 500);
    REQUIRE(eval_of(whiteUpBlackMoves) < -500);

    // Black up a queen, Black to move -> strongly positive for Black (the mover).
    const std::string blackUp = "3qk3/8/8/8/8/8/8/4K3 b - - 0 1";
    REQUIRE(eval_of(blackUp) > 500);
}

// ---------------------------------------------------------------------------
// Zobrist en-passant carry-forward fix.
// ---------------------------------------------------------------------------
TEST_CASE("ep hash: phantom ep square hashes equal to no ep", "[eval][zobrist]") {
    // After 1.e4 the ep square is e3, but no black pawn can capture it, so the
    // key must equal the same board with no ep square at all.
    Position withPhantom = Position::startpos();
    withPhantom.make_move(Move(E2, E4));

    Position noEp;
    noEp.set_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");

    REQUIRE(withPhantom.key() == noEp.key());
}

TEST_CASE("ep hash: capturable ep differs from no ep", "[eval][zobrist]") {
    // Black pawn on d4 can answer e2e4 with ...dxe3, so the ep file IS hashed.
    Position withEp;
    withEp.set_fen("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");

    Position noEp;
    noEp.set_fen("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");

    REQUIRE(withEp.key() != noEp.key());
}

TEST_CASE("ep hash: different move orders, identical board, equal keys", "[eval][zobrist]") {
    // Order A: 1.e4 e5 2.Nf3 -> ep cleared by the knight move.
    Position a = Position::startpos();
    a.make_move(Move(E2, E4));
    a.make_move(Move(E7, E5));
    a.make_move(Move(G1, F3));

    // Order B: 1.Nf3 e5 2.e4 -> phantom ep on e3 (no black pawn can take it).
    Position b = Position::startpos();
    b.make_move(Move(G1, F3));
    b.make_move(Move(E7, E5));
    b.make_move(Move(E2, E4));

    // Same pieces, same side to move; the only difference is B's phantom ep,
    // which the fix excludes from the key -> the two keys must match.
    REQUIRE(a.fen().substr(0, a.fen().find(' ')) ==
            b.fen().substr(0, b.fen().find(' ')));   // identical placement
    REQUIRE(a.key() == b.key());
}
