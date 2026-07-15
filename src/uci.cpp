#include "uci.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
#include "sync.hpp"
#include "tt.hpp"
#include "zobrist.hpp"

// ===========================================================================
// uci.cpp
// ===========================================================================

namespace {

constexpr char ENGINE_NAME[]   = "Grimfang";
constexpr char ENGINE_AUTHOR[] = "shywolf91";

// Hash UCI option bounds (MB).
constexpr int HASH_DEFAULT = 16;
constexpr int HASH_MIN     = 1;
constexpr int HASH_MAX     = 1024;

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
    // Declaration order matters for teardown: the worker thread references
    // pos_ and stop_ via `this`, so worker_ is declared LAST and therefore
    // destroyed FIRST -- the thread is always torn down before the state it
    // touches, even on an abnormal path.
    Position          pos_ = Position::startpos();
    std::atomic<bool> stop_{ false };
    std::thread       worker_;

    // Signal stop, then join the worker. Idempotent: safe to call when no
    // search is running or when the worker has already finished naturally.
    void ensure_idle() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable())
            worker_.join();
    }

    void start_search(const Search::Limits& limits) {
        // Never assign over a joinable thread (that would std::terminate); a
        // previous search must already be joined. ensure_idle() precedes every
        // start in run(), but join defensively here too.
        if (worker_.joinable())
            worker_.join();
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

    // Parse `setoption name <Name> value <V>`. Only `Hash` is functional.
    void cmd_setoption(std::istringstream& ss) {
        std::string token, name, value;
        if (!(ss >> token) || token != "name")
            return;

        // The name may contain spaces; collect tokens until "value".
        while (ss >> token && token != "value")
            name += (name.empty() ? "" : " ") + token;
        if (token == "value")
            std::getline(ss >> std::ws, value);

        if (name == "Hash") {
            try {
                int mb = std::stoi(value);
                mb = std::max(HASH_MIN, std::min(HASH_MAX, mb));
                TT.resize(static_cast<std::size_t>(mb));
            } catch (...) {
                // Ignore a malformed value; keep the current table.
            }
        }
        // Other options are accepted and ignored.
    }

    void cmd_go(std::istringstream& ss) {
        Search::Limits limits;
        bool sawClock = false;
        std::string token;
        while (ss >> token) {
            if      (token == "wtime")     { ss >> limits.time[WHITE]; sawClock = true; }
            else if (token == "btime")     { ss >> limits.time[BLACK]; sawClock = true; }
            else if (token == "winc")      { ss >> limits.inc[WHITE];  sawClock = true; }
            else if (token == "binc")      { ss >> limits.inc[BLACK];  sawClock = true; }
            else if (token == "movestogo") ss >> limits.movestogo;
            else if (token == "depth")     ss >> limits.depth;
            else if (token == "nodes")     ss >> limits.nodes;
            else if (token == "movetime")  { ss >> limits.movetime; sawClock = true; }
            else if (token == "infinite")  limits.infinite = true;
            // "ponder" is accepted and ignored (no real pondering yet).
        }

        // Degenerate clock (e.g. `go wtime 0 btime 0 winc 0 binc 0`): a clock
        // was named but every field is zero and no other bound applies. Force a
        // tiny finite budget so we always return a legal move promptly instead
        // of searching unbounded; the time manager floors it to MIN_THINK_MS.
        if (sawClock && !limits.infinite && limits.depth == 0 && limits.nodes == 0
            && limits.movetime == 0
            && limits.time[WHITE] == 0 && limits.time[BLACK] == 0
            && limits.inc[WHITE]  == 0 && limits.inc[BLACK]  == 0) {
            limits.movetime = 1;
        }

        start_search(limits);
    }
};

void UciLoop::run() {
    std::cout.setf(std::ios::unitbuf);   // line-ish flushing for GUIs

    TT.resize(HASH_DEFAULT);             // default table until `setoption Hash`

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string token;
        if (!(ss >> token))
            continue;

        if (token == "uci") {
            std::ostringstream oss;
            oss << "id name " << ENGINE_NAME << '\n'
                << "id author " << ENGINE_AUTHOR << '\n'
                << "option name Hash type spin default " << HASH_DEFAULT
                << " min " << HASH_MIN << " max " << HASH_MAX << '\n'
                << "option name Threads type spin default 1 min 1 max 1\n"
                << "uciok\n";
            sync_cout(oss.str());
        } else if (token == "isready") {
            // Answered while a search may be running -> must be line-atomic.
            sync_cout("readyok\n");
        } else if (token == "setoption") {
            ensure_idle();
            cmd_setoption(ss);
        } else if (token == "ucinewgame") {
            ensure_idle();
            TT.clear();
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
        } else if (token == "bench") {
            ensure_idle();
            int d = Search::BENCH_DEFAULT_DEPTH;
            int override = 0;
            if (ss >> override) d = override;   // optional depth override
            Search::bench(d);
        } else if (token == "eval") {
            ensure_idle();
            sync_cout("eval " + std::to_string(Eval::evaluate(pos_)) + " (cp, side to move)\n");
        } else if (token == "d" || token == "print") {
            ensure_idle();
            sync_cout(pos_.fen() + "\n");
        } else {
            sync_cout("unknown command: " + token + "\n");
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
