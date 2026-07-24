#include "datagen.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "bitboard.hpp"
#include "movegen.hpp"
#include "position.hpp"
#include "search.hpp"

// ===========================================================================
// datagen.cpp - self-play data generation implementation.
// ===========================================================================

namespace {

constexpr int SCORE_BLOWOUT_LIMIT = 3000;

// Abort the run if this many games fail in a row. A persistently failing
// play_one_game (e.g. every opening is degenerate, or the search never returns
// a legal move) must not spin the completion loop forever.
constexpr int MAX_CONSECUTIVE_FAILURES = 10000;

// xorshift64* PRNG for reproducible random opening moves — the same generator
// used for Zobrist keys (zobrist.cpp) and the magic search (attacks.cpp).
//
// This replaces the previous 64-bit LCG. An LCG's low-order bits have short
// periods (the lowest bit just alternates), and bounded() reduces with a
// modulo that is dominated by those low bits — biasing which legal move is
// picked for each random opening ply and shrinking opening diversity in the
// generated training data. xorshift64*'s output is well-mixed across all bits,
// so the modulo is far closer to uniform. The sequence is still fully
// deterministic for a given seed, so runs remain reproducible.
struct Rng64 {
    std::uint64_t state;

    explicit Rng64(std::uint64_t seed) : state(seed ? seed : 1) {}

    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }

    int bounded(int n) {
        return static_cast<int>(next() % static_cast<std::uint64_t>(n));
    }
};

std::uint8_t piece_to_datagen(Piece pc) {
    if (pc == NO_PIECE)
        return 0;
    const Color     c = color_of(pc);
    const PieceType t = type_of(pc);
    return static_cast<std::uint8_t>((c == WHITE ? 1 : 7) + static_cast<int>(t));
}

Piece datagen_to_piece(std::uint8_t p) {
    if (p == 0)
        return NO_PIECE;
    if (p >= 1 && p <= 6)
        return make_piece(WHITE, static_cast<PieceType>(p - 1));
    if (p >= 7 && p <= 12)
        return make_piece(BLACK, static_cast<PieceType>(p - 7));
    return NO_PIECE;
}

std::uint8_t encode_castling(CastlingRights cr) {
    std::uint8_t bits = 0;
    if (cr & WHITE_OO)  bits |= 1;
    if (cr & WHITE_OOO) bits |= 2;
    if (cr & BLACK_OO)  bits |= 4;
    if (cr & BLACK_OOO) bits |= 8;
    return bits;
}

CastlingRights decode_castling(std::uint8_t bits) {
    CastlingRights cr = NO_CASTLING;
    if (bits & 1) cr = cr | WHITE_OO;
    if (bits & 2) cr = cr | WHITE_OOO;
    if (bits & 4) cr = cr | BLACK_OO;
    if (bits & 8) cr = cr | BLACK_OOO;
    return cr;
}

bool is_insufficient_material(const Position& pos) {
    const Bitboard occ = pos.pieces();
    if (occ == pos.pieces(KING))
        return true;   // K vs K

    const Bitboard wMinors = pos.pieces(WHITE, KNIGHT, BISHOP);
    const Bitboard bMinors = pos.pieces(BLACK, KNIGHT, BISHOP);
    const Bitboard minors  = wMinors | bMinors;

    if (occ == (pos.pieces(WHITE, KING) | pos.pieces(BLACK, KING) | minors)) {
        // Only kings and minor pieces remain.
        if (wMinors == 0 && bMinors == 0)
            return true;   // K vs K (already caught) or bare kings
        if (wMinors == 0 && popcount(bMinors) == 1)
            return true;   // K vs K+B or K vs K+N
        if (bMinors == 0 && popcount(wMinors) == 1)
            return true;
        if (popcount(wMinors) == 1 && popcount(bMinors) == 1
            && type_of(pos.piece_on(lsb(wMinors))) == BISHOP
            && type_of(pos.piece_on(lsb(bMinors))) == BISHOP) {
            // K+B vs K+B on same color bishops is dead draw in practice;
            // treat as insufficient for adjudication throughput.
            const Square ws = lsb(wMinors);
            const Square bs = lsb(bMinors);
            if ((static_cast<int>(file_of(ws)) + static_cast<int>(rank_of(ws))) % 2
                == (static_cast<int>(file_of(bs)) + static_cast<int>(rank_of(bs))) % 2)
                return true;
        }
    }
    return false;
}

bool is_game_over_draw(const Position& pos) {
    return pos.is_draw() || is_insufficient_material(pos);
}

enum class TerminalKind {
    None,
    WhiteMate,
    BlackMate,
    Stalemate,
    Draw,
};

TerminalKind terminal_state(const Position& pos) {
    MoveList legal;
    generate_legal(pos, legal);
    if (legal.count == 0) {
        if (pos.checkers())
            return pos.side_to_move() == WHITE ? TerminalKind::BlackMate
                                              : TerminalKind::WhiteMate;
        return TerminalKind::Stalemate;
    }
    if (is_game_over_draw(pos))
        return TerminalKind::Draw;
    return TerminalKind::None;
}

DatagenOutcome outcome_from_terminal(TerminalKind kind) {
    switch (kind) {
        case TerminalKind::WhiteMate: return DatagenOutcome::WhiteWin;
        case TerminalKind::BlackMate: return DatagenOutcome::BlackWin;
        default:                      return DatagenOutcome::Draw;
    }
}

DatagenOutcome outcome_from_resign(Color sideToMove, Value score) {
    if (score > 0)
        return sideToMove == WHITE ? DatagenOutcome::WhiteWin : DatagenOutcome::BlackWin;
    if (score < 0)
        return sideToMove == WHITE ? DatagenOutcome::BlackWin : DatagenOutcome::WhiteWin;
    return DatagenOutcome::Draw;
}

Move pick_random_move(Position& pos, Rng64& rng) {
    MoveList list;
    generate_legal(pos, list);
    // Guard against an empty move list before the modulo: a terminal position
    // has no legal moves, and `next() % 0` is undefined behavior (divide by
    // zero). Return MOVE_NONE and let the caller discard the game.
    if (list.count == 0)
        return MOVE_NONE;
    return list.moves[rng.bounded(list.count)];
}

bool play_random_opening(Position& pos, Rng64& rng, int randomPlies) {
    for (int i = 0; i < randomPlies; ++i) {
        const TerminalKind t = terminal_state(pos);
        if (t != TerminalKind::None)
            return false;   // degenerate opening -> discard game

        const Move m = pick_random_move(pos, rng);
        if (!m.is_ok())
            return false;   // no legal move -> discard game
        pos.make_move(m);
    }
    return terminal_state(pos) == TerminalKind::None;
}

struct BufferedPosition {
    DatagenRecord rec;
};

std::size_t flush_game(std::vector<BufferedPosition>& buffer, DatagenOutcome outcome,
                       std::FILE* out, const std::string& shardPath) {
    const std::size_t n = buffer.size();
    for (auto& bp : buffer) {
        apply_result(bp.rec, static_cast<Color>(bp.rec.stm), outcome);
        // fwrite returns the number of whole records written; anything short of
        // 1 (a partial write or an I/O error) means the shard is now corrupt.
        // Abort the whole run instead of continuing to emit a truncated shard.
        if (std::fwrite(&bp.rec, sizeof(DatagenRecord), 1, out) != 1) {
            const int         err = errno;
            const std::string why =
                err ? std::system_category().message(err) : "unknown error";
            std::fprintf(stderr,
                         "datagen: short write / I/O error writing shard '%s' "
                         "(%s); aborting run rather than producing a corrupt shard\n",
                         shardPath.c_str(), why.c_str());
            std::fflush(stderr);
            throw std::runtime_error("short write to shard: " + shardPath);
        }
    }
    buffer.clear();
    return n;
}

bool play_one_game(Position& pos, Rng64& rng, const DatagenOptions& opts,
                   std::vector<BufferedPosition>& buffer, std::FILE* out,
                   std::uint64_t& positions) {
    pos = Position::startpos();
    buffer.clear();

    if (!play_random_opening(pos, rng, opts.randomPlies))
        return false;

    int resignRun = 0;

    for (;;) {
        const TerminalKind terminal = terminal_state(pos);
        if (terminal != TerminalKind::None) {
            positions += flush_game(buffer, outcome_from_terminal(terminal), out,
                                    opts.outPath);
            return true;
        }

        std::atomic<bool> stop{ false };
        const Search::Result sr =
            Search::search_nodes(pos, opts.nodeCap, stop);

        const Value score = sr.score;
        const Move  move  = sr.bestMove;
        if (!move.is_ok())
            return false;

        if (should_record_position(pos, score)) {
            BufferedPosition bp;
            bp.rec = encode_record(pos, score);
            buffer.push_back(bp);
        }

        if (score >= opts.resignScore || score <= -opts.resignScore) {
            ++resignRun;
            if (resignRun >= opts.resignPlies) {
                positions += flush_game(buffer,
                                        outcome_from_resign(pos.side_to_move(), score),
                                        out, opts.outPath);
                return true;
            }
        } else {
            resignRun = 0;
        }

        pos.make_move(move);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public encode/decode / predicates
// ---------------------------------------------------------------------------
DatagenRecord encode_record(const Position& pos, Value searchScore) {
    DatagenRecord rec{};
    rec.version = DATAGEN_VERSION;
    for (Square s = A1; s <= H8; s = static_cast<Square>(s + 1))
        rec.piece[s] = piece_to_datagen(pos.piece_on(s));

    rec.stm      = static_cast<std::uint8_t>(pos.side_to_move());
    rec.castling = encode_castling(pos.castling_rights());
    rec.epSquare = (pos.ep_square() == SQ_NONE)
                       ? static_cast<std::int8_t>(-1)
                       : static_cast<std::int8_t>(pos.ep_square());
    rec.score    = static_cast<std::int16_t>(std::clamp(static_cast<int>(searchScore),
                                                        -32768, 32767));
    rec.result   = 0;
    std::memset(rec.reserved, 0, sizeof(rec.reserved));
    return rec;
}

void apply_result(DatagenRecord& rec, Color stm, DatagenOutcome outcome) {
    rec.result = result_for_stm(stm, outcome);
}

std::int8_t result_for_stm(Color stm, DatagenOutcome outcome) {
    switch (outcome) {
        case DatagenOutcome::Draw:     return 1;
        case DatagenOutcome::WhiteWin: return (stm == WHITE) ? 2 : 0;
        case DatagenOutcome::BlackWin: return (stm == BLACK) ? 2 : 0;
    }
    return 1;
}

bool should_record_position(const Position& pos, Value searchScore) {
    if (pos.checkers() != 0)
        return false;
    if (std::abs(searchScore) > SCORE_BLOWOUT_LIMIT)
        return false;
    return true;
}

std::string record_to_fen(const DatagenRecord& rec) {
    Position pos;
    std::string fen;

    // Piece placement
    for (int r = RANK_8; r >= RANK_1; --r) {
        int empties = 0;
        for (int f = FILE_A; f <= FILE_H; ++f) {
            const Square sq = make_square(static_cast<File>(f), static_cast<Rank>(r));
            const std::uint8_t dg = rec.piece[sq];
            if (dg == 0) {
                ++empties;
            } else {
                if (empties) { fen += std::to_string(empties); empties = 0; }
                // Datagen codes 1..6=white P..K, 7..12=black P..K -> chars index dg-1.
                constexpr char chars[] = "PNBRQKpnbrqk";
                fen += chars[dg - 1];
            }
        }
        if (empties) fen += std::to_string(empties);
        if (r != RANK_1) fen += '/';
    }

    fen += (rec.stm == 0) ? " w " : " b ";

    const CastlingRights cr = decode_castling(rec.castling);
    if (cr == NO_CASTLING) {
        fen += '-';
    } else {
        if (cr & WHITE_OO)  fen += 'K';
        if (cr & WHITE_OOO) fen += 'Q';
        if (cr & BLACK_OO)  fen += 'k';
        if (cr & BLACK_OOO) fen += 'q';
    }

    fen += ' ';
    if (rec.epSquare < 0)
        fen += '-';
    else {
        const Square ep = static_cast<Square>(rec.epSquare);
        fen += static_cast<char>('a' + file_of(ep));
        fen += static_cast<char>('1' + rank_of(ep));
    }

    fen += " 0 1";
    return fen;
}

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------
DatagenOptions parse_datagen_args(int argc, char** argv) {
    DatagenOptions opts;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* flag) -> std::string {
            if (arg != flag || i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + flag);
            return argv[++i];
        };

        if      (arg == "--games")         opts.games       = std::stoi(needValue("--games"));
        else if (arg == "--out")           opts.outPath     = needValue("--out");
        else if (arg == "--seed")          opts.seed        = std::stoull(needValue("--seed"));
        else if (arg == "--nodes")         opts.nodeCap     = std::stoull(needValue("--nodes"));
        else if (arg == "--random-plies")  opts.randomPlies = std::stoi(needValue("--random-plies"));
        else if (arg == "--resign-score")  opts.resignScore = std::stoi(needValue("--resign-score"));
        else if (arg == "--resign-plies")  opts.resignPlies = std::stoi(needValue("--resign-plies"));
        else if (arg == "--progress")      opts.progress    = std::stoi(needValue("--progress"));
        else
            throw std::runtime_error("unknown datagen flag: " + arg);
    }

    if (opts.games <= 0)
        throw std::runtime_error("--games must be positive");
    if (opts.nodeCap == 0)
        throw std::runtime_error("--nodes must be positive");
    if (opts.randomPlies < 0)
        throw std::runtime_error("--random-plies must be >= 0");
    if (opts.resignScore <= 0)
        throw std::runtime_error("--resign-score must be positive");
    if (opts.resignPlies <= 0)
        throw std::runtime_error("--resign-plies must be positive");
    if (opts.progress < 0)
        throw std::runtime_error("--progress must be >= 0");
    return opts;
}

// ---------------------------------------------------------------------------
// Main datagen driver
// ---------------------------------------------------------------------------
std::uint64_t run_datagen(const DatagenOptions& opts) {
    std::FILE* out = std::fopen(opts.outPath.c_str(), "wb");
    if (!out)
        throw std::runtime_error("cannot open output file: " + opts.outPath);

    Position                      pos;
    Rng64                         rng(opts.seed);
    std::vector<BufferedPosition> buffer;
    buffer.reserve(256);

    int           completed = 0;
    int           consecutiveFailures = 0;
    std::uint64_t positions = 0;
    while (completed < opts.games) {
        if (play_one_game(pos, rng, opts, buffer, out, positions)) {
            ++completed;
            consecutiveFailures = 0;
            if (opts.progress > 0 && completed % opts.progress == 0) {
                std::cout << "shard progress: " << completed << " games, "
                          << positions << " positions\n";
                std::cout.flush();
            }
        } else if (++consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            // A persistently failing play_one_game would otherwise spin this
            // loop forever without ever completing a game.
            std::fclose(out);
            throw std::runtime_error(
                "datagen aborted: " + std::to_string(MAX_CONSECUTIVE_FAILURES)
                + " consecutive game failures (last completed: "
                + std::to_string(completed) + " of " + std::to_string(opts.games)
                + ")");
        }
    }

    std::fclose(out);

    std::ifstream check(opts.outPath, std::ios::binary | std::ios::ate);
    const auto bytes = check.tellg();
    if (bytes < 0 || bytes % static_cast<std::streamoff>(sizeof(DatagenRecord)) != 0)
        throw std::runtime_error("output file size is not a multiple of record size");

    positions = static_cast<std::uint64_t>(bytes) / sizeof(DatagenRecord);

    std::cout << "datagen complete: " << completed << " games, "
              << positions << " positions -> " << opts.outPath << std::endl;
    return positions;
}
