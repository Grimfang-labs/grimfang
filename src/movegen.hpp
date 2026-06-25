#pragma once

#include "move.hpp"
#include "types.hpp"

// ===========================================================================
// movegen.hpp - fully-legal staged move generation (implemented in sub-step 3).
// ===========================================================================

class Position;

// Fixed-capacity, heap-free move buffer.
struct MoveList {
    static constexpr int CAPACITY = 256;

    Move moves[CAPACITY];
    int  count = 0;

    void add(Move m) { moves[count++] = m; }

    Move*       begin()       { return moves; }
    Move*       end()         { return moves + count; }
    const Move* begin() const { return moves; }
    const Move* end()   const { return moves + count; }
    int         size()  const { return count; }
};

// Generate every fully-legal move for the side to move (implemented sub-step 3).
void generate_legal(const Position& pos, MoveList& list);
