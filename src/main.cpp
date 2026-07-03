#include <iostream>
#include <string>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "datagen.hpp"
#include "search.hpp"
#include "uci.hpp"
#include "zobrist.hpp"

// ===========================================================================
// main.cpp - dispatch into the UCI loop. The Stage 1 perft CLI now lives as a
// debug command inside the loop (perft / divide).
//
// `stockwolf bench [depth]` runs the search signature directly (OpenBench
// convention) without entering the interactive loop.
//
// `stockwolf datagen ...` runs self-play data generation (NNUE arc, prompt 1).
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

    if (argc > 1 && std::string(argv[1]) == "datagen") {
        BB::init();
        Zobrist::init();
        Attacks::init();

        try {
            const DatagenOptions opts = parse_datagen_args(argc, argv);
            run_datagen(opts);
        } catch (const std::exception& ex) {
            std::cerr << "datagen error: " << ex.what() << std::endl;
            return 1;
        }
        return 0;
    }

    UCI::loop();
    return 0;
}
