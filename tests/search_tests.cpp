#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "eval.hpp"
#include "move.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "zobrist.hpp"

// ===========================================================================
// search_tests.cpp - Stage 2 sub-step 2 gate.
//
//   * generate_captures consistency vs the legal generator
//   * mate detection (mate in 1 / 2 / 3) with correct ply-adjusted scores
//   * draw scoring (repetition + fifty-move)
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

bool is_capture_or_promo(const Position& pos, Move m) {
    if (m.type_of() == PROMOTION || m.type_of() == EN_PASSANT) return true;
    return pos.piece_on(m.to_sq()) != NO_PIECE;
}

std::vector<std::uint16_t> raw_sorted(const MoveList& list) {
    std::vector<std::uint16_t> v;
    for (int i = 0; i < list.count; ++i) v.push_back(list.moves[i].raw());
    std::sort(v.begin(), v.end());
    return v;
}

// Convert a search score to a (signed) mate distance in moves, or 0 if not mate.
int mate_in(Value score) {
    if (score >= VALUE_MATE_IN_MAX_PLY)
        return (VALUE_MATE - score + 1) / 2;
    if (score <= -VALUE_MATE_IN_MAX_PLY)
        return -((VALUE_MATE + score + 1) / 2);
    return 0;
}

bool is_checkmate(const std::string& fen) {
    Position pos;
    pos.set_fen(fen);
    MoveList list;
    generate_legal(pos, list);
    return list.count == 0 && pos.checkers() != 0;
}

} // namespace

TEST_CASE("generate_captures matches the legal capture/promotion subset", "[search][movegen]") {
    const std::vector<std::string> fens = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 1",  // ep available
        "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1",                                // in check
    };

    for (const auto& fen : fens) {
        Position pos;
        pos.set_fen(fen);

        MoveList caps;
        generate_captures(pos, caps);

        MoveList legal;
        generate_legal(pos, legal);
        MoveList expected;
        for (int i = 0; i < legal.count; ++i)
            if (is_capture_or_promo(pos, legal.moves[i]))
                expected.add(legal.moves[i]);

        REQUIRE(raw_sorted(caps) == raw_sorted(expected));
    }
}

TEST_CASE("search: mate in 1", "[search][mate]") {
    Position pos;
    pos.set_fen("7k/Q7/6K1/8/8/8/8/8 w - - 0 1");

    const Search::Result r = Search::search_fixed(pos, 4);
    REQUIRE(mate_in(r.score) == 1);
    REQUIRE(r.bestMove != MOVE_NONE);

    // The reported best move must actually deliver checkmate.
    pos.make_move(r.bestMove);
    REQUIRE(is_checkmate(pos.fen()));
}

TEST_CASE("qsearch: mate-in-1 at depth 1 (no stand-pat in check)",
          "[search][qsearch][soundness]") {
    // Depth 1 plays the mating move then enters qsearch with the side to move
    // in check and zero legal evasions. Old qsearch stood pat on the static
    // eval there and could miss mate-in-1 entirely; bestScore = -MATE + ply
    // makes the checkmated child return a mate score.
    Position pos;
    pos.set_fen("7k/Q7/6K1/8/8/8/8/8 w - - 0 1");
    TT.clear();
    const Search::Result r = Search::search_fixed(pos, 1);
    REQUIRE(mate_in(r.score) == 1);
    REQUIRE(r.bestMove != MOVE_NONE);
    pos.make_move(r.bestMove);
    REQUIRE(is_checkmate(pos.fen()));
}

TEST_CASE("search: mate in 2", "[search][mate]") {
    // White: Kf6, Rf1. Black: Kh8. 1.Kg6! Kg8 2.Rf8#. No mate in 1 (Rf8+ Kh7).
    Position pos;
    pos.set_fen("7k/8/5K2/8/8/8/8/5R2 w - - 0 1");

    const Search::Result r = Search::search_fixed(pos, 6);
    REQUIRE(mate_in(r.score) == 2);
}

TEST_CASE("search: mate in 3", "[search][mate]") {
    // Standard mate-in-3 study (White to play): 1.Nc5-a6+ and mate follows.
    Position pos;
    pos.set_fen("1k5r/pP3ppp/3p2b1/1BN1n3/1Q2P3/P1B5/KP3P1P/7q w - - 1 0");

    const Search::Result r = Search::search_fixed(pos, 7);
    REQUIRE(mate_in(r.score) == 3);
}

TEST_CASE("aspiration: terminates and matches full-window on mates; sharp FENs legal",
          "[search][aspiration]") {
    // Aspiration changes cost, not the mathematical answer — but TT entries
    // written by a fail-high/low re-search can change move ordering on the
    // final pass, so non-mate scores need not match bit-identically. Mate
    // positions either skip aspiration (mate guard) or agree on mate distance.
    const struct MateCase {
        const char* fen;
        int         depth;
        int         mateMoves;
    } mates[] = {
        { "7k/Q7/6K1/8/8/8/8/8 w - - 0 1", 5, 1 },
        { "7k/8/5K2/8/8/8/8/5R2 w - - 0 1", 6, 2 },
    };

    for (const auto& c : mates) {
        Position posAsp;
        posAsp.set_fen(c.fen);
        TT.clear();
        const Search::Result asp = Search::search_fixed(posAsp, c.depth, true);

        Position posFull;
        posFull.set_fen(c.fen);
        TT.clear();
        const Search::Result full = Search::search_fixed(posFull, c.depth, false);

        REQUIRE(mate_in(asp.score) == c.mateMoves);
        REQUIRE(asp.score == full.score);
        REQUIRE(asp.bestMove == full.bestMove);
    }

    // Sharp tactical FENs: loop must terminate with a legal bestmove, and must
    // not thrash (aspirated node count should not greatly exceed full-window).
    const struct SharpCase {
        const char* fen;
        int         depth;
    } sharp[] = {
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 6 },
        { "r1bqk2r/ppp2ppp/2n2n2/2bpp3/4P3/2NP1N2/PPP2PPP/R1BQKB1R w KQkq - 0 5", 6 },
        { "8/pp3p1k/2p2q1p/3r1P2/5R2/7P/P1P1QP2/7K b - - 2 30", 6 },
    };

    for (const auto& c : sharp) {
        Position posAsp;
        posAsp.set_fen(c.fen);
        TT.clear();
        const Search::Result asp = Search::search_fixed(posAsp, c.depth, true);

        Position posFull;
        posFull.set_fen(c.fen);
        TT.clear();
        const Search::Result full = Search::search_fixed(posFull, c.depth, false);

        REQUIRE(asp.bestMove != MOVE_NONE);
        REQUIRE(asp.depth == c.depth);

        MoveList legal;
        generate_legal(posAsp, legal);
        bool legalChoice = false;
        for (int i = 0; i < legal.count; ++i)
            if (legal.moves[i] == asp.bestMove) { legalChoice = true; break; }
        REQUIRE(legalChoice);

        // Thrash guard: repeated fail-high/low would inflate nodes badly.
        REQUIRE(asp.nodes <= full.nodes * 2 + 1000);
    }
}

TEST_CASE("search: stalemate scores as a draw", "[search][draw]") {
    // Black to move, no legal moves, not in check -> the search returns 0.
    Position pos;
    pos.set_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");

    const Search::Result r = Search::search_fixed(pos, 4);
    REQUIRE(r.score == 0);
    REQUIRE(r.bestMove == MOVE_NONE);
}

TEST_CASE("draw: threefold repetition is a draw", "[search][draw]") {
    Position pos = Position::startpos();
    REQUIRE_FALSE(pos.is_draw());

    pos.make_move(Move(G1, F3));
    pos.make_move(Move(G8, F6));
    pos.make_move(Move(F3, G1));
    pos.make_move(Move(F6, G8));   // back to the start position, same side to move

    REQUIRE(pos.is_draw());
}

TEST_CASE("draw: fifty-move rule", "[search][draw]") {
    Position pos;
    pos.set_fen("4k3/8/8/8/8/8/8/4K3 w - - 100 1");
    REQUIRE(pos.is_draw());
}

TEST_CASE("self-play smoke: full games stay legal and terminate", "[search][selfplay]") {
    const std::vector<std::string> openings = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    };

    for (const auto& fen : openings) {
        Position pos;
        pos.set_fen(fen);

        for (int ply = 0; ply < 120; ++ply) {
            if (pos.is_draw())
                break;

            MoveList legal;
            generate_legal(pos, legal);
            if (legal.count == 0)
                break;   // checkmate or stalemate ends the game

            const Search::Result r = Search::search_fixed(pos, 4);
            REQUIRE(r.bestMove != MOVE_NONE);

            bool legalChoice = false;
            for (int i = 0; i < legal.count; ++i)
                if (legal.moves[i] == r.bestMove) { legalChoice = true; break; }
            REQUIRE(legalChoice);

            pos.make_move(r.bestMove);
        }
        SUCCEED();   // reached the end of the game without crashing
    }
}
