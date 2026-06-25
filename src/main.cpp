#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "perft.hpp"
#include "position.hpp"
#include "zobrist.hpp"

// ===========================================================================
// main.cpp - minimal CLI driver for perft verification (no UCI yet).
//
// Commands:
//   position startpos
//   position fen <FEN>
//   perft <n>
//   divide <n>
//   quit
// ===========================================================================

namespace {

void run_perft(Position& pos, int depth) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const std::uint64_t nodes = perft(pos, depth, /*bulk=*/false);
    const auto end = clock::now();

    const double secs =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const double nps = secs > 0.0 ? static_cast<double>(nodes) / secs : 0.0;

    std::cout << "nodes " << nodes
              << "  time " << secs << "s"
              << "  nps " << static_cast<std::uint64_t>(nps) << '\n';
}

} // namespace

int main() {
    BB::init();
    Zobrist::init();
    Attacks::init();

    Position pos = Position::startpos();

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "quit" || token == "exit") {
            break;
        } else if (token == "position") {
            std::string kind;
            ss >> kind;
            if (kind == "startpos") {
                pos = Position::startpos();
            } else if (kind == "fen") {
                std::string fen, part;
                while (ss >> part) { fen += part; fen += ' '; }
                if (!fen.empty()) fen.pop_back();
                pos.set_fen(fen);
            }
            std::cout << pos.fen() << '\n';
        } else if (token == "perft") {
            int d = 0;
            if (ss >> d) run_perft(pos, d);
        } else if (token == "divide") {
            int d = 0;
            if (ss >> d) perft_divide(pos, d);
        } else if (token == "fen") {
            std::cout << pos.fen() << '\n';
        } else if (!token.empty()) {
            std::cout << "unknown command: " << token << '\n';
        }
    }
    return 0;
}
