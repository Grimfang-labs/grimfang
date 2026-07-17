#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "score.hpp"
#include "types.hpp"

class Position;

// ===========================================================================
// datagen.hpp - self-play data generation for NNUE training (NNUE arc 1/3).
//
// Writes a stream of packed DatagenRecord structs to a shard file. Positions
// within a game are buffered until the outcome is known, then backfilled with
// an STM-relative result and flushed. This mode is separate from UCI and does
// not alter normal search/bench behavior.
// ===========================================================================

#pragma pack(push, 1)
struct DatagenRecord {
    std::uint8_t version;      // = 1
    std::uint8_t piece[64];    // a1..h8; 0=empty, 1..6=W P..K, 7..12=B P..K
    std::uint8_t stm;          // 0=white, 1=black
    std::uint8_t castling;     // bit0=WK bit1=WQ bit2=BK bit3=BQ
    std::int8_t  epSquare;     // -1 if none, else 0..63
    std::int16_t score;        // cp, STM-relative, from the search that chose the move
    std::int8_t  result;       // 0=loss for stm, 1=draw, 2=win for stm
    std::uint8_t reserved[3];  // zero-filled
};
#pragma pack(pop)

static_assert(sizeof(DatagenRecord) == 74,
              "DatagenRecord size is part of the on-disk contract for prompt 2");

constexpr std::uint8_t DATAGEN_VERSION = 1;

enum class DatagenOutcome : std::int8_t {
    WhiteWin = 0,   // white delivered mate / adjudicated win
    Draw     = 1,
    BlackWin = 2,
};

struct DatagenOptions {
    int           games        = 100;
    std::string   outPath      = "datagen.bin";
    std::uint64_t seed         = 1;
    std::uint64_t nodeCap      = 5000;
    int           randomPlies  = 8;
    int           resignScore  = 1500;
    int           resignPlies  = 4;
    int           progress     = 0;   // every N completed games; 0 = off
};

// Encode/decode helpers (testable without running a full game).
DatagenRecord encode_record(const Position& pos, Value searchScore);
void          apply_result(DatagenRecord& rec, Color stm, DatagenOutcome outcome);
std::string   record_to_fen(const DatagenRecord& rec);

// Recording predicates (exported for unit tests).
bool should_record_position(const Position& pos, Value searchScore);

// STM-relative result: 0=loss, 1=draw, 2=win for the given side.
std::int8_t result_for_stm(Color stm, DatagenOutcome outcome);

// Run the self-play datagen loop; returns total positions written.
std::uint64_t run_datagen(const DatagenOptions& opts);

// Parse `argv[2..]` after the `datagen` subcommand. Throws on bad input.
DatagenOptions parse_datagen_args(int argc, char** argv);
