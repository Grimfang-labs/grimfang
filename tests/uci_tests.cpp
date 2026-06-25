#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

// ===========================================================================
// uci_tests.cpp - Stage 2 sub-step 4 gate.
//
// Drives the real engine binary (STOCKWOLF_ENGINE) by piping a scripted command
// sequence to its stdin and inspecting stdout: handshake, position/go, time
// bounds, and asynchronous stop.
// ===========================================================================

#ifndef STOCKWOLF_ENGINE
#error "STOCKWOLF_ENGINE path must be defined by the build"
#endif

namespace {

std::atomic<unsigned> g_counter{ 0 };

// Run the engine with `input` on stdin, returning everything it wrote to stdout.
std::string run_engine(const std::string& input) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path();
    const unsigned id = g_counter.fetch_add(1);
    const fs::path inPath  = dir / ("sw_in_"  + std::to_string(id) + ".txt");
    const fs::path outPath = dir / ("sw_out_" + std::to_string(id) + ".txt");

    {
        std::ofstream in(inPath, std::ios::binary);
        in << input;
    }

    // Wrap the whole command in an extra pair of quotes so cmd.exe keeps the
    // quoted paths intact even when they contain spaces.
    const std::string inner = "\"" + std::string(STOCKWOLF_ENGINE) + "\" < \""
                            + inPath.string() + "\" > \"" + outPath.string() + "\"";
    const std::string full  = "\"" + inner + "\"";
    std::system(full.c_str());

    std::ifstream out(outPath, std::ios::binary);
    std::stringstream buf;
    buf << out.rdbuf();

    std::error_code ec;
    fs::remove(inPath, ec);
    fs::remove(outPath, ec);
    return buf.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Extract the token following the first "bestmove " in `out`, or "" if absent.
std::string bestmove_of(const std::string& out) {
    const auto pos = out.find("bestmove ");
    if (pos == std::string::npos) return "";
    std::istringstream ss(out.substr(pos));
    std::string tag, mv;
    ss >> tag >> mv;
    return mv;
}

int count_occurrences(const std::string& s, const std::string& sub) {
    int n = 0;
    for (std::size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size()))
        ++n;
    return n;
}

} // namespace

TEST_CASE("uci: handshake", "[uci]") {
    const std::string out = run_engine("uci\nquit\n");
    REQUIRE(contains(out, "id name StockWolf"));
    REQUIRE(contains(out, "id author"));
    REQUIRE(contains(out, "uciok"));
}

TEST_CASE("uci: isready", "[uci]") {
    const std::string out = run_engine("isready\nquit\n");
    REQUIRE(contains(out, "readyok"));
}

TEST_CASE("uci: position + go depth yields one legal bestmove", "[uci]") {
    const std::string out =
        run_engine("position startpos moves e2e4\ngo depth 6\nquit\n");

    REQUIRE(count_occurrences(out, "bestmove ") == 1);

    const std::string mv = bestmove_of(out);
    REQUIRE(mv.size() >= 4);
    // A black reply after 1.e4: a legal move from a black-occupied square.
    REQUIRE((mv != "0000"));
}

TEST_CASE("uci: go movetime returns within a sane time bound", "[uci]") {
    const auto start = std::chrono::steady_clock::now();
    const std::string out = run_engine("position startpos\ngo movetime 200\nquit\n");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();

    REQUIRE(contains(out, "bestmove "));
    REQUIRE(bestmove_of(out) != "0000");
    REQUIRE(elapsed < 5000);   // generous upper bound incl. process startup
}

TEST_CASE("uci: stop during go infinite returns a bestmove", "[uci]") {
    const std::string out =
        run_engine("position startpos\ngo infinite\nstop\nquit\n");
    REQUIRE(contains(out, "bestmove "));
    REQUIRE(bestmove_of(out) != "0000");
}
