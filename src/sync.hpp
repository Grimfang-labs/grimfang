#pragma once

#include <iostream>
#include <mutex>
#include <string>

// ===========================================================================
// sync.hpp - line-atomic stdout.
//
// The search runs on a worker thread that emits `info` and `bestmove` lines
// while the UCI thread may concurrently emit replies (notably `readyok`, which
// must be answered while a search is running). Concurrent use of std::cout is
// free of data races per the standard, but the individual `<<` operations can
// interleave mid-line. Routing every whole-line write through one mutex keeps
// output line-atomic so a GUI never sees a spliced line.
// ===========================================================================

inline std::mutex& io_mutex() {
    static std::mutex m;
    return m;
}

// Write one already-assembled line (caller includes any trailing '\n').
inline void sync_cout(const std::string& line) {
    std::lock_guard<std::mutex> lock(io_mutex());
    std::cout << line;
}
