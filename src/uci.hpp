#pragma once

// ===========================================================================
// uci.hpp - the UCI command loop.
//
// Reads stdin line by line, runs the search on a worker thread (so `stop`
// works asynchronously), and also accepts the non-UCI debug commands
// `perft`, `divide`, `eval` and `bench`.
// ===========================================================================

namespace UCI {

// Initialise the engine tables and run the command loop until `quit`/EOF.
void loop();

} // namespace UCI
