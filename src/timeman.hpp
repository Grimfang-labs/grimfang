#pragma once

#include <cstdint>

#include "search.hpp"
#include "types.hpp"

// ===========================================================================
// timeman.hpp - turn `go` parameters into a time budget.
//
//   * soft limit: checked between iterative-deepening iterations (don't start
//     a new depth past it).
//   * hard limit: aborts the in-progress search.
// `useTime == false` means no clock was given (bounded by depth/nodes/stop).
// ===========================================================================

struct TimeBudget {
    bool         useTime = false;
    std::int64_t soft    = 0;
    std::int64_t hard    = 0;   // milliseconds
};

TimeBudget compute_budget(const Search::Limits& limits, Color us);
