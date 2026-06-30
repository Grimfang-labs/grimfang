#pragma once

#include <cstdint>

// ===========================================================================
// score.hpp - shared search/eval value constants.
//
// Scores are centipawn-ish integers from the side-to-move's perspective
// (positive = good for the side to move). Mate scores are stored ply-adjusted:
// a mate delivered at search ply `p` is `VALUE_MATE - p`, so shallower mates
// score higher (the engine prefers faster mates and slower self-mates).
// ===========================================================================

using Value = std::int32_t;

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_DRAW     = 0;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_MATE     = 32000;

// Sentinel for "no value" (e.g. an uncached static eval in a TT entry). Chosen
// to fit in an int16_t and to lie outside every real score range.
constexpr Value VALUE_NONE     = 32002;

// Maximum search depth / ply bound. Sizes the triangular PV table.
constexpr int MAX_PLY = 128;

// Any |score| at or above this is a forced mate (vs a normal evaluation).
constexpr Value VALUE_MATE_IN_MAX_PLY = VALUE_MATE - MAX_PLY;
