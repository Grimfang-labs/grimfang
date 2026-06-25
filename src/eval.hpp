#pragma once

#include "score.hpp"

// ===========================================================================
// eval.hpp - static position evaluation.
//
// Tapered material + piece-square tables (PeSTO starting constants). The score
// is returned from the side-to-move's perspective (positive = good for the
// side to move), which is mandatory for negamax.
// ===========================================================================

class Position;

namespace Eval {

// Static, side-relative evaluation of `pos` in centipawns.
Value evaluate(const Position& pos);

} // namespace Eval
