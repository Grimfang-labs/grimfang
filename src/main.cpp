#include <string>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "search.hpp"
#include "uci.hpp"
#include "zobrist.hpp"

// ===========================================================================
// main.cpp - dispatch into the UCI loop. The Stage 1 perft CLI now lives as a
// debug command inside the loop (perft / divide).
//
// `stockwolf bench [depth]` runs the search signature directly (OpenBench
// convention) without entering the interactive loop.
// ===========================================================================

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "bench") {
        BB::init();
        Zobrist::init();
        Attacks::init();

        int depth = Search::BENCH_DEFAULT_DEPTH;
        if (argc > 2)
            depth = std::stoi(argv[2]);
        Search::bench(depth);
        return 0;
    }

    UCI::loop();
    return 0;
}
