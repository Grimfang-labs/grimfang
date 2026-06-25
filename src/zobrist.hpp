#pragma once

#include "types.hpp"

// ===========================================================================
// zobrist.hpp - reproducible random keys for incremental hashing.
// ===========================================================================

namespace Zobrist {

extern Key psq[PIECE_NB][SQ_NB];   // [piece][square]
extern Key side;                    // XOR'd when it is Black to move
extern Key castling[CASTLING_NB];   // indexed by the 4-bit rights mask
extern Key enpassant[FILE_NB];      // indexed by the ep file

// Seed a fixed PRNG and fill the tables. Idempotent.
void init();

} // namespace Zobrist
