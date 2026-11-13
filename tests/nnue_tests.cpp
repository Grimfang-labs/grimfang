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
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 164 },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", -147 },
    { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 42 },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 82 },
    { "8/8/8/4k3/8/3K4/4P3/8 b - - 0 1", -381 },
    { "rnbqkbnr/pp1ppppp/8/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq c6 0 3", 173 },
    { "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 676 },
    { "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1", 1635 },
    { "4k3/8/8/8/8/8/4Q3/4K3 b - - 0 1", -1042 },
    { "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", 1778 },
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

namespace {

// Small deterministic PRNG (xorshift64*) so the playouts are reproducible.
struct TestRng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

nnue::AccumulatorPair refreshed(const Position& pos) {
    nnue::AccumulatorPair acc;
    nnue::refresh(acc, pos);
    return acc;
}

void require_accumulators_match_refresh(const Position& pos) {
    REQUIRE(pos.nnue_accumulators().byPerspective == refreshed(pos).byPerspective);
    REQUIRE(nnue::evaluate(pos) == nnue::evaluate_scalar(pos));
}

// Prefer the move kinds that exercise every update shape: castling (2 adds,
// 2 subs), en passant / captures / capture-promotions (1 add, 2 subs) and
// promotions (piece swap).
Move pick_move(const MoveList& moves, const Position& pos, TestRng& rng) {
    MoveList special;
    for (int i = 0; i < moves.count; ++i) {
        const Move m = moves.moves[i];
        if (m.type_of() != NORMAL || !pos.empty(m.to_sq()))
            special.moves[special.count++] = m;
    }
    if (special.count > 0 && rng.below(2) == 0)
        return special.moves[rng.below(special.count)];
    return moves.moves[rng.below(moves.count)];
}

const std::array<const char*, 6> kPlayoutSeeds {{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r3k2r/ppp2ppp/8/3Pp3/8/8/PPP2PPP/R3K2R w KQkq e6 0 1",
    "4k3/1P4P1/8/2pP4/8/8/1p4p1/4K3 w - c6 0 1",
}};

} // namespace

TEST_CASE("nnue: fused SIMD update is bit-identical to scalar", "[nnue][simd]") {
    TestRng rng { 0x9E3779B97F4A7C15ULL };
    Position pos;
    pos.set_fen(kPlayoutSeeds[1]);
    const nnue::AccumulatorPair parent = refreshed(pos);

    // Every update shape the kernels specialise, plus the empty (null) update.
    const std::array<std::pair<int, int>, 4> shapes {{ { 1, 1 }, { 1, 2 }, { 2, 2 }, { 0, 0 } }};
    for (const auto& [adds, subs] : shapes) {
        for (int trial = 0; trial < 50; ++trial) {
            nnue::AccumulatorUpdate update;
            for (int a = 0; a < adds; ++a)
                update.add(static_cast<Piece>(rng.below(2) * 8 + rng.below(6)),
                           static_cast<Square>(rng.below(64)));
            for (int s = 0; s < subs; ++s)
                update.sub(static_cast<Piece>(rng.below(2) * 8 + rng.below(6)),
                           static_cast<Square>(rng.below(64)));
            nnue::AccumulatorPair simdChild;
            nnue::AccumulatorPair scalarChild;
            nnue::apply_update(simdChild, parent, update);
            nnue::apply_update_scalar(scalarChild, parent, update);
            REQUIRE(simdChild.byPerspective == scalarChild.byPerspective);
            if (adds == 0 && subs == 0)
                REQUIRE(simdChild.byPerspective == parent.byPerspective);
        }
    }

    // And along real games, the lazily built accumulators equal the scalar
    // refresh after every move.
    for (const char* fen : kPlayoutSeeds) {
        pos.set_fen(fen);
        for (int ply = 0; ply < 40; ++ply) {
            MoveList moves;
            generate_legal(pos, moves);
            if (moves.count == 0)
                break;
            pos.make_move(pick_move(moves, pos, rng));
            nnue::AccumulatorPair scalarAcc;
            nnue::refresh_scalar(scalarAcc, pos);
            REQUIRE(pos.nnue_accumulators().byPerspective == scalarAcc.byPerspective);
        }
    }
}

TEST_CASE("nnue: black_feature mirrors white_feature for every piece and square", "[nnue]") {
    for (int c = 0; c < COLOR_NB; ++c) {
        for (int pt = PAWN; pt <= KING; ++pt) {
            for (int sq = 0; sq < SQ_NB; ++sq) {
                const Piece  pc = make_piece(static_cast<Color>(c), static_cast<PieceType>(pt));
                const Square s  = static_cast<Square>(sq);
                // Black perspective: own pieces are BLACK, board flipped.
                const int expected = (c == BLACK ? 0 : 384) + 64 * pt + (sq ^ 56);
                REQUIRE(nnue::black_feature(nnue::white_feature(pc, s)) == expected);
            }
        }
    }
}

TEST_CASE("nnue: lazy accumulators match refresh over random playouts with null moves and unmakes",
          "[nnue]") {
    enum class Step { Move, Null };
    struct Done { Step step; Move move; };

    TestRng rng { 0xD1B54A32D192ED03ULL };
    std::uint64_t checks = 0;
    for (const char* fen : kPlayoutSeeds) {
        for (int game = 0; game < 4; ++game) {
            Position pos;
            pos.set_fen(fen);
            std::vector<Done> done;

            const auto undo_one = [&]() {
                const Done d = done.back();
                done.pop_back();
                if (d.step == Step::Null) pos.undo_null_move();
                else                      pos.unmake_move(d.move);
            };

            for (int action = 0; action < 300; ++action) {
                const int roll = rng.below(100);
                if (roll < 25 && !done.empty()) {
                    // Unmake a burst of 1..6 plies (possibly below the deepest
                    // computed ply) without evaluating in between.
                    for (int n = 1 + rng.below(6); n > 0 && !done.empty(); --n)
                        undo_one();
                } else if (roll < 35 && pos.checkers() == 0) {
                    pos.do_null_move();
                    done.push_back({ Step::Null, MOVE_NONE });
                } else {
                    MoveList moves;
                    generate_legal(pos, moves);
                    if (moves.count == 0 || done.size() > 200) {
                        if (!done.empty()) undo_one();
                        continue;
                    }
                    const Move m = pick_move(moves, pos, rng);
                    pos.make_move(m);
                    done.push_back({ Step::Move, m });
                }

                // Evaluate only at random points so lazy chains build up.
                if (rng.below(3) == 0) {
                    require_accumulators_match_refresh(pos);
                    ++checks;
                }

                // Occasionally copy the position mid-playout (with pending,
                // un-materialised plies) and check the copies are exact.
                if (rng.below(40) == 0) {
                    const Position copy = pos;
                    require_accumulators_match_refresh(copy);
                    Position assigned;
                    assigned = pos;
                    require_accumulators_match_refresh(assigned);
                    ++checks;
                }
            }

            while (!done.empty()) {
                undo_one();
                if (rng.below(4) == 0)
                    require_accumulators_match_refresh(pos);
            }
            require_accumulators_match_refresh(pos);
        }
    }
    REQUIRE(checks > 1000);
}

TEST_CASE("nnue: copies stay exact while both sides keep moving and unmaking", "[nnue]") {
    TestRng rng { 7 };
    Position pos = Position::startpos();
    std::vector<Move> played;
    for (int ply = 0; ply < 30; ++ply) {
        MoveList moves;
        generate_legal(pos, moves);
        const Move m = pick_move(moves, pos, rng);
        pos.make_move(m);
        played.push_back(m);
        if (ply == 10)
            require_accumulators_match_refresh(pos);   // computed up to ply 10 only
    }

    // The copy carries the pending plies 11..29 and can unmake below them.
    Position copy = pos;
    for (int i = 0; i < 25; ++i) {
        copy.unmake_move(played[played.size() - 1 - static_cast<std::size_t>(i)]);
        require_accumulators_match_refresh(copy);
    }
    // The original is untouched by the copy's work.
    require_accumulators_match_refresh(pos);
}

TEST_CASE("nnue: long un-evaluated move sequences and deep unmakes stay exact", "[nnue]") {
    TestRng rng { 42 };
    Position pos = Position::startpos();
    std::vector<Move> played;

    // 120 plies with no eval at all: longer than the lazy-walk limit, so the
    // first eval takes the full-refresh fallback.
    for (int ply = 0; ply < 120; ++ply) {
        MoveList moves;
        generate_legal(pos, moves);
        if (moves.count == 0)
            break;
        const Move m = moves.moves[rng.below(moves.count)];
        pos.make_move(m);
        played.push_back(m);
    }
    require_accumulators_match_refresh(pos);

    // Unwind in uneven chunks, evaluating at each stop: intermediate plies were
    // never materialised, so each eval walks back to a computed ancestor or
    // refreshes.
    while (!played.empty()) {
        for (int n = 1 + rng.below(25); n > 0 && !played.empty(); --n) {
            pos.unmake_move(played.back());
            played.pop_back();
        }
        require_accumulators_match_refresh(pos);
    }
    REQUIRE(pos.fen() == Position::startpos().fen());
}

TEST_CASE("nnue: accumulator stack grows past its preallocation", "[nnue]") {
    // Shuffle knights far beyond the preallocated 2*MAX_PLY plies so the stack
    // has to grow (and reallocate) mid-sequence, then unwind it all.
    Position pos = Position::startpos();
    const std::array<Move, 4> cycle {{
        Move::make<NORMAL>(G1, F3), Move::make<NORMAL>(G8, F6),
        Move::make<NORMAL>(F3, G1), Move::make<NORMAL>(F6, G8),
    }};
    const int plies = 4 * MAX_PLY + 7;
    for (int i = 0; i < plies; ++i) {
        pos.make_move(cycle[static_cast<std::size_t>(i % 4)]);
        if (i % 3 == 0)
            require_accumulators_match_refresh(pos);
    }
    require_accumulators_match_refresh(pos);
    for (int i = plies - 1; i >= 0; --i) {
        pos.unmake_move(cycle[static_cast<std::size_t>(i % 4)]);
        if (i % 53 == 0)
            require_accumulators_match_refresh(pos);
    }
    require_accumulators_match_refresh(pos);
}
