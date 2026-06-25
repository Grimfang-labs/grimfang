#include <iostream>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "zobrist.hpp"

// ===========================================================================
// main.cpp - CLI driver for perft verification.
//
// Sub-step 1: initialise the engine tables and report readiness. The full
// command loop (position / perft / divide / quit) is wired up in sub-step 3.
// ===========================================================================

int main() {
    BB::init();
    Zobrist::init();
    Attacks::init();

    std::cout << "stockwolf stage1 - core initialised\n";
    return 0;
}
