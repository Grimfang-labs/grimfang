#include "zobrist.hpp"

// ===========================================================================
// zobrist.cpp - fill the hash key tables from a fixed-seed xorshift64* PRNG so
// builds are fully reproducible.
// ===========================================================================

namespace Zobrist {

Key psq[PIECE_NB][SQ_NB];
Key side;
Key castling[CASTLING_NB];
Key enpassant[FILE_NB];

namespace {

// xorshift64* — deterministic, decent distribution, no library dependency.
class PRNG {
public:
    explicit PRNG(std::uint64_t seed) : s_(seed) {}

    Key next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }

private:
    std::uint64_t s_;
};

} // namespace

void init() {
    PRNG rng(0x9D39247E33776D41ULL);

    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < SQ_NB; ++s)
            psq[p][s] = rng.next();

    side = rng.next();

    for (int c = 0; c < CASTLING_NB; ++c)
        castling[c] = rng.next();

    for (int f = 0; f < FILE_NB; ++f)
        enpassant[f] = rng.next();
}

} // namespace Zobrist
