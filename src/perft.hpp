#pragma once

#include <cstdint>
#include <string>

// ===========================================================================
// perft.hpp - perft, perft-divide and the EPD suite runner (sub-step 3).
// ===========================================================================

class Position;

// Count leaf nodes at the given depth. With bulk = true the leaf layer is
// counted as movelist.size() at depth 1 (faster once correctness is proven);
// the default exercises make/unmake all the way down.
std::uint64_t perft(Position& pos, int depth, bool bulk = false);

// Print each root move and its child node count, then the total.
void perft_divide(Position& pos, int depth);

// Run an EPD suite ("fen ;Dn count ;Dm count ...") and report pass/fail.
bool perft_suite(const std::string& epdPath);
