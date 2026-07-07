#pragma once

#include "score.hpp"

// ===========================================================================
// eval.hpp - search-facing static evaluation entry point.
//
// The engine now uses the embedded NNUE net for all static evaluations. The
// return value remains side-to-move relative centipawns so the search code's
// negamax sign convention is unchanged.
// ===========================================================================

class Position;

namespace Eval {

// Static, side-relative evaluation of `pos` in centipawns.
Value evaluate(const Position& pos);

} // namespace Eval
