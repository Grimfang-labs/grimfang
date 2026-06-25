#include "uci.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "attacks.hpp"
#include "bitboard.hpp"
#include "eval.hpp"
#include "movegen.hpp"
#include "perft.hpp"
#include "position.hpp"
#include "search.hpp"
#include "zobrist.hpp"

// ===========================================================================
// uci.cpp
// ===========================================================================

namespace {

constexpr char ENGINE_NAME[]    = "StockWolf";
constexpr char ENGINE_VERSION[] = "2.0";
constexpr char ENGINE_AUTHOR[]  = "shywolf91";

void run_perft(Position& pos, int depth) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const std::uint64_t nodes = perft(pos, depth, /*bulk=*/false);
    const auto end = clock::now();

    const double secs =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const std::uint64_t nps =
        secs > 0.0 ? static_cast<std::uint64_t>(static_cast<double>(nodes) / secs) : 0;

    std::cout << "nodes " << nodes << " time " << secs << "s nps " << nps << std::endl;
}

class UciLoop {
public:
    void run();

private:
    Position          pos_ = Position::startpos();
    std::thread       worker_;
    std::atomic<bool> stop_{ false };

    // Stop any running search and join the worker thread.
    void ensure_idle() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable())
            worker_.join();
    }

    void start_search(const Search::Limits& limits) {
        stop_.store(false, std::memory_order_relaxed);
        worker_ = std::thread([this, limits]() {
            Search::search(pos_, limits, stop_);
        });
    }

    Move parse_move(const std::string& uci) const {
        MoveList list;
        generate_legal(pos_, list);
        for (int i = 0; i < list.count; ++i)
            if (to_uci(list.moves[i]) == uci)
                return list.moves[i];
        return MOVE_NONE;
    }

    void apply_moves(std::istringstream& ss) {
        std::string mv;
        while (ss >> mv) {
            const Move m = parse_move(mv);
            if (!m.is_ok())
                break;
            pos_.make_move(m);
        }
    }

    void cmd_position(std::istringstream& ss) {
        std::string kind;
        if (!(ss >> kind))
            return;

        std::string token;
        if (kind == "startpos") {
            pos_ = Position::startpos();
        } else if (kind == "fen") {
            std::string fen;
            while (ss >> token && token != "moves")
                fen += token + " ";
            pos_.set_fen(fen);
            if (token == "moves") {
                apply_moves(ss);
                return;
            }
            return;
        } else {
            return;
        }

        if (ss >> token && token == "moves")
            apply_moves(ss);
    }

    void cmd_go(std::istringstream& ss) {
        Search::Limits limits;
        std::string token;
        while (ss >> token) {
            if      (token == "wtime")     ss >> limits.time[WHITE];
            else if (token == "btime")     ss >> limits.time[BLACK];
            else if (token == "winc")      ss >> limits.inc[WHITE];
            else if (token == "binc")      ss >> limits.inc[BLACK];
            else if (token == "movestogo") ss >> limits.movestogo;
            else if (token == "depth")     ss >> limits.depth;
            else if (token == "nodes")     ss >> limits.nodes;
            else if (token == "movetime")  ss >> limits.movetime;
            else if (token == "infinite")  limits.infinite = true;
            // "ponder" is accepted and ignored (no real pondering yet).
        }
        start_search(limits);
    }
};

void UciLoop::run() {
    std::cout.setf(std::ios::unitbuf);   // line-ish flushing for GUIs

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string token;
        if (!(ss >> token))
            continue;

        if (token == "uci") {
            std::cout << "id name " << ENGINE_NAME << ' ' << ENGINE_VERSION << '\n';
            std::cout << "id author " << ENGINE_AUTHOR << '\n';
            std::cout << "option name Hash type spin default 16 min 1 max 1024\n";
            std::cout << "option name Threads type spin default 1 min 1 max 1\n";
            std::cout << "uciok" << std::endl;
        } else if (token == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (token == "setoption") {
            // No functional options this stage; parse gracefully and ignore.
        } else if (token == "ucinewgame") {
            ensure_idle();
            pos_ = Position::startpos();
        } else if (token == "position") {
            ensure_idle();
            cmd_position(ss);
        } else if (token == "go") {
            ensure_idle();
            cmd_go(ss);
        } else if (token == "stop") {
            ensure_idle();
        } else if (token == "ponderhit") {
            // Accept as a no-op transition (no real pondering yet).
        } else if (token == "quit" || token == "exit") {
            ensure_idle();
            break;
        } else if (token == "perft") {
            ensure_idle();
            int d = 0;
            if (ss >> d) run_perft(pos_, d);
        } else if (token == "divide") {
            ensure_idle();
            int d = 0;
            if (ss >> d) perft_divide(pos_, d);
        } else if (token == "eval") {
            ensure_idle();
            std::cout << "eval " << Eval::evaluate(pos_) << " (cp, side to move)" << std::endl;
        } else if (token == "d" || token == "print") {
            ensure_idle();
            std::cout << pos_.fen() << std::endl;
        } else {
            std::cout << "unknown command: " << token << std::endl;
        }
    }

    ensure_idle();
}

} // namespace

void UCI::loop() {
    BB::init();
    Zobrist::init();
    Attacks::init();

    UciLoop loop;
    loop.run();
}
