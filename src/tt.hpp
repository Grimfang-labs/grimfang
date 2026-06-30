#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "move.hpp"
#include "score.hpp"
#include "types.hpp"

// ===========================================================================
// tt.hpp - shared transposition table (Stage 3, rung 3.1).
//
// A position is hashed by its 64-bit Zobrist key: the low bits select a
// cluster (the cluster count is a power of two so indexing is a mask), the
// upper 32 bits are stored in the entry to verify against index collisions.
//
// MATE-SCORE CORRECTION (the #1 TT bug): mate scores are distance-from-root,
// but the table is shared across nodes at different root distances. Stored
// mate values are therefore made node-relative on write (value_to_tt) and
// re-rooted on read (value_from_tt).
// ===========================================================================

enum Bound : std::uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,                              // value is an upper bound
    BOUND_LOWER = 2,                              // value is a lower bound
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER       // = 3
};

// Adjust a mate score for storage (node-relative) / retrieval (root-relative).
Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply);

// One transposition-table entry. 12 bytes; ClusterSize entries per cluster.
struct TTEntry {
    std::uint32_t key32;        // upper 32 bits of the Zobrist key (verification)
    std::uint16_t move16;       // best / refutation move
    std::int16_t  value16;      // mate-corrected search score
    std::int16_t  eval16;       // cached static eval (VALUE_NONE if absent)
    std::uint8_t  depth8;       // remaining depth this entry was searched to
    std::uint8_t  genBound8;    // bits 0-1: bound, bits 2-7: generation

    Move  move()  const { return Move(move16); }
    Value value() const { return static_cast<Value>(value16); }
    Value eval()  const { return static_cast<Value>(eval16); }
    int   depth() const { return static_cast<int>(depth8); }
    Bound bound() const { return static_cast<Bound>(genBound8 & 0x3); }
    bool  is_empty() const { return bound() == BOUND_NONE; }
};

class TranspositionTable {
public:
    // Allocate `mb` megabytes (rounded down to a power-of-two cluster count) and
    // clear. A resize is a no-op fast path when the size is unchanged.
    void resize(std::size_t mb);

    // Zero every entry (does not change the current generation).
    void clear();

    // Advance the generation/age counter. Call once at the start of each search.
    void new_search() { generation8_ += GENERATION_DELTA; }

    std::uint8_t generation() const { return generation8_; }
    std::size_t  cluster_count() const { return clusterCount_; }

    // Find a matching entry in the cluster. Returns true and yields the entry
    // when a stored entry verifies against `key`; otherwise returns false.
    bool probe(Key key, TTEntry*& out);

    // Write an entry with the replacement scheme, applying value_to_tt(v, ply).
    void store(Key key, Move m, Value v, Value eval, int depth, Bound b, int ply);

    // Approximate fill level (per mille) sampled over the first 1000 entries:
    // the fraction whose generation equals the current generation.
    int hashfull() const;

private:
    static constexpr int ClusterSize = 3;
    struct Cluster { TTEntry entry[ClusterSize]; };

    // Generation occupies the top 6 bits of genBound8; the bound the low 2.
    static constexpr unsigned GENERATION_DELTA = 0x4;
    static constexpr unsigned GENERATION_MASK  = 0xFC;
    static constexpr unsigned GENERATION_CYCLE = 0xFF + GENERATION_DELTA;

    Cluster* cluster_at(Key key) {
        return &table_[static_cast<std::size_t>(static_cast<std::uint32_t>(key)) & mask_];
    }

    // How stale `genBound8` is relative to the current generation (wraps).
    unsigned relative_age(std::uint8_t genBound8) const {
        return (GENERATION_CYCLE + generation8_ - genBound8) & GENERATION_MASK;
    }

    std::vector<Cluster> table_;
    std::size_t          clusterCount_ = 0;
    std::size_t          mask_         = 0;
    std::uint8_t         generation8_  = 0;
};

// The single engine-wide table.
extern TranspositionTable TT;
