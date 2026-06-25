#include "perft.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "movegen.hpp"
#include "position.hpp"

// ===========================================================================
// perft.cpp - move-path enumeration for correctness verification.
// ===========================================================================

std::uint64_t perft(Position& pos, int depth, bool bulk) {
    if (depth == 0)
        return 1;

    MoveList list;
    generate_legal(pos, list);

    // Bulk counting: trust the legal generator at the leaf layer. Off by
    // default so make/unmake bugs are still exercised all the way down.
    if (bulk && depth == 1)
        return static_cast<std::uint64_t>(list.size());

    std::uint64_t nodes = 0;
    for (Move m : list) {
        pos.make_move(m);
        nodes += perft(pos, depth - 1, bulk);
        pos.unmake_move(m);
    }
    return nodes;
}

void perft_divide(Position& pos, int depth) {
    MoveList list;
    generate_legal(pos, list);

    std::uint64_t total = 0;
    for (Move m : list) {
        pos.make_move(m);
        const std::uint64_t n = (depth <= 1) ? 1 : perft(pos, depth - 1, false);
        pos.unmake_move(m);
        std::cout << to_uci(m) << ": " << n << '\n';
        total += n;
    }
    std::cout << "\nMoves:  " << list.size() << '\n';
    std::cout << "Nodes:  " << total << '\n';
}

namespace {

// Parse one EPD line: "<fen> ;D1 20 ;D2 400 ...". Returns false on a blank or
// comment line.
struct PerftCase {
    std::string                                    fen;
    std::vector<std::pair<int, std::uint64_t>>     depths;
};

bool parse_epd_line(const std::string& line, PerftCase& out) {
    if (line.empty() || line[0] == '#')
        return false;

    const std::size_t semi = line.find(';');
    if (semi == std::string::npos)
        return false;

    out.fen = line.substr(0, semi);
    // Trim trailing whitespace from the FEN.
    while (!out.fen.empty() && (out.fen.back() == ' ' || out.fen.back() == '\t'))
        out.fen.pop_back();

    out.depths.clear();
    std::size_t pos = semi;
    while ((pos = line.find('D', pos)) != std::string::npos) {
        std::istringstream seg(line.substr(pos + 1));
        int            depth = 0;
        std::uint64_t  count = 0;
        if (seg >> depth >> count)
            out.depths.emplace_back(depth, count);
        ++pos;
    }
    return !out.depths.empty();
}

} // namespace

bool perft_suite(const std::string& epdPath) {
    std::ifstream in(epdPath);
    if (!in) {
        std::cerr << "perft_suite: cannot open " << epdPath << '\n';
        return false;
    }

    bool        allOk = true;
    std::string line;
    while (std::getline(in, line)) {
        PerftCase tc;
        if (!parse_epd_line(line, tc))
            continue;

        Position pos;
        pos.set_fen(tc.fen);
        std::cout << tc.fen << '\n';

        for (const auto& [depth, expected] : tc.depths) {
            const std::uint64_t got = perft(pos, depth, true);
            const bool          ok  = (got == expected);
            allOk = allOk && ok;
            std::cout << "  D" << depth << ": " << got
                      << (ok ? "  OK" : "  FAIL (expected " + std::to_string(expected) + ")")
                      << '\n';
        }
    }
    return allOk;
}
