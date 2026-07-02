#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "position.hpp"
#include "types.hpp"
#include "zobrist.hpp"

// ===========================================================================
// nullmove_tests.cpp - Stage 3 rung 3.5 gate (null-move facility).
//
// The classic NMP disaster is a stale ep square or a wrong Zobrist key
// corrupting the TT. These tests kill both:
//   * do_null_move(); undo_null_move(); restores a byte-identical position
//     (FEN) and an identical incremental key.
//   * after do_null_move(), the incremental key equals a from-scratch
//     compute_key() of the resulting (flipped-stm, ep-cleared) position.
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

const std::vector<std::string> kFens = {
    // startpos
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    // capturable ep: white pawn on d5 can take c6 en passant
    "rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq c6 0 3",
    // non-capturable ep: a6 is set but no white pawn attacks it
    "rnbqkbnr/1ppppppp/8/p7/8/8/PPPPPPPP/RNBQKBNR w KQkq a6 0 2",
    // full castling rights, both sides
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    // black to move, partial rights, a middlegame-ish position
    "r1bqk2r/ppp2ppp/2n2n2/2bpp3/4P3/2NP1N2/PPP2PPP/R1BQKB1R b KQkq - 0 5",
    // black to move with a capturable ep square (white just played d2-d4)
    "rnbqkbnr/ppp1pppp/8/8/2pP4/8/PP2PPPP/RNBQKBNR b KQkq d3 0 3",
};

} // namespace

TEST_CASE("null move: do/undo restores position and key byte-identically", "[nullmove]") {
    for (const auto& fen : kFens) {
        Position pos;
        pos.set_fen(fen);

        const std::string fenBefore = pos.fen();
        const Key         keyBefore = pos.key();
        const Color       stmBefore = pos.side_to_move();

        pos.do_null_move();

        // Side flipped, ep cleared, and the incremental key is exact.
        REQUIRE(pos.side_to_move() == ~stmBefore);
        REQUIRE(pos.ep_square() == SQ_NONE);
        REQUIRE(pos.key() == pos.compute_key());
        REQUIRE(pos.key() != keyBefore);          // side hash always flips it

        pos.undo_null_move();

        REQUIRE(pos.fen() == fenBefore);
        REQUIRE(pos.key() == keyBefore);
        REQUIRE(pos.key() == pos.compute_key());
    }
}

TEST_CASE("null move: clearing a capturable ep removes its key component", "[nullmove]") {
    // Same position with vs without the capturable ep square differ only by the
    // ep hash; after a null move (which clears ep) both must share one key.
    Position withEp;
    withEp.set_fen("rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq c6 0 3");
    REQUIRE(withEp.ep_square() != SQ_NONE);

    Position noEp;
    noEp.set_fen("rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq - 0 3");

    // The ep square makes the keys differ before the null move...
    REQUIRE(withEp.key() != noEp.key());

    withEp.do_null_move();
    noEp.do_null_move();

    // ...but after passing (ep dropped), the resulting positions are identical.
    REQUIRE(withEp.key() == noEp.key());
    REQUIRE(withEp.key() == withEp.compute_key());
}

TEST_CASE("null move: has_non_pawn_material zugzwang guard", "[nullmove]") {
    // King + pawn endgame: neither side has a piece -> guard must be false.
    Position kp;
    kp.set_fen("8/8/p7/8/8/1P6/8/K6k w - - 0 1");
    REQUIRE_FALSE(kp.has_non_pawn_material(WHITE));
    REQUIRE_FALSE(kp.has_non_pawn_material(BLACK));

    // Startpos: both sides have pieces.
    Position sp = Position::startpos();
    REQUIRE(sp.has_non_pawn_material(WHITE));
    REQUIRE(sp.has_non_pawn_material(BLACK));

    // Lone knight vs bare king: only the knight side has material.
    Position n;
    n.set_fen("8/8/8/4k3/8/8/4N3/4K3 w - - 0 1");
    REQUIRE(n.has_non_pawn_material(WHITE));
    REQUIRE_FALSE(n.has_non_pawn_material(BLACK));
}
