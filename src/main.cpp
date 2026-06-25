#include "uci.hpp"

// ===========================================================================
// main.cpp - dispatch into the UCI loop. The Stage 1 perft CLI now lives as a
// debug command inside the loop (perft / divide).
// ===========================================================================

int main() {
    UCI::loop();
    return 0;
}
