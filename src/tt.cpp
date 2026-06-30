#include "tt.hpp"

#include <cstring>

// ===========================================================================
// tt.cpp - transposition table implementation.
// ===========================================================================

TranspositionTable TT;

// ---------------------------------------------------------------------------
// Mate-score correction. Mate scores (|v| >= VALUE_MATE_IN_MAX_PLY) encode a
// distance from the *root*. Because the table is shared between nodes reached
// at different root distances (transpositions, and entirely separate searches
// re-probing the same slot), a stored mate must be made relative to the node
// that stored it and re-rooted at the node that reads it.
// ---------------------------------------------------------------------------
Value value_to_tt(Value v, int ply) {
    if (v >=  VALUE_MATE_IN_MAX_PLY) return v + ply;
    if (v <= -VALUE_MATE_IN_MAX_PLY) return v - ply;
    return v;
}

Value value_from_tt(Value v, int ply) {
    if (v >=  VALUE_MATE_IN_MAX_PLY) return v - ply;
    if (v <= -VALUE_MATE_IN_MAX_PLY) return v + ply;
    return v;
}

// ---------------------------------------------------------------------------
// Sizing & lifecycle
// ---------------------------------------------------------------------------
void TranspositionTable::resize(std::size_t mb) {
    const std::size_t bytes    = mb * 1024 * 1024;
    std::size_t       clusters = bytes / sizeof(Cluster);

    // Round the cluster count down to a power of two so indexing is a mask.
    std::size_t pow2 = 1;
    while (pow2 * 2 <= clusters)
        pow2 *= 2;
    clusters = pow2;

    if (clusters == clusterCount_) {
        clear();
        return;
    }

    clusterCount_ = clusters;
    mask_         = clusterCount_ - 1;
    table_.assign(clusterCount_, Cluster{});   // value-init zeroes every entry
    generation8_  = 0;
}

void TranspositionTable::clear() {
    if (clusterCount_ == 0)
        return;
    std::memset(table_.data(), 0, clusterCount_ * sizeof(Cluster));
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------
bool TranspositionTable::probe(Key key, TTEntry*& out) {
    out = nullptr;
    if (clusterCount_ == 0)
        return false;

    const std::uint32_t k32 = static_cast<std::uint32_t>(key >> 32);
    Cluster*            c   = cluster_at(key);

    for (int i = 0; i < ClusterSize; ++i) {
        TTEntry& e = c->entry[i];
        if (e.key32 == k32 && !e.is_empty()) {
            // Refresh the age so frequently used entries survive replacement.
            e.genBound8 = static_cast<std::uint8_t>(generation8_ | e.bound());
            out = &e;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Store (with replacement)
// ---------------------------------------------------------------------------
void TranspositionTable::store(Key key, Move m, Value v, Value eval,
                               int depth, Bound b, int ply) {
    if (clusterCount_ == 0)
        return;

    const std::uint32_t k32 = static_cast<std::uint32_t>(key >> 32);
    Cluster*            c   = cluster_at(key);
    TTEntry*            tte = c->entry;

    TTEntry* target = nullptr;

    // 1) An entry for this exact position already exists -> refresh it.
    for (int i = 0; i < ClusterSize; ++i)
        if (tte[i].key32 == k32 && !tte[i].is_empty()) { target = &tte[i]; break; }

    // 2) Otherwise prefer an empty slot.
    if (!target)
        for (int i = 0; i < ClusterSize; ++i)
            if (tte[i].is_empty()) { target = &tte[i]; break; }

    // 3) Otherwise evict the least valuable entry: keep deeper + current-gen
    //    entries, so shallow and/or stale entries are overwritten first.
    if (!target) {
        target = &tte[0];
        for (int i = 1; i < ClusterSize; ++i)
            if (tte[i].depth8 - static_cast<int>(relative_age(tte[i].genBound8)) * 2
                < target->depth8 - static_cast<int>(relative_age(target->genBound8)) * 2)
                target = &tte[i];
    }

    const bool sameSlot = (target->key32 == k32) && !target->is_empty();

    // Preserve an existing refutation move when the new store carries none.
    if (m.is_ok() || !sameSlot)
        target->move16 = m.raw();

    // For the same position, keep a deeper search result rather than clobbering
    // it with a shallower one (an exact bound always wins).
    if (b == BOUND_EXACT || !sameSlot || depth + 4 > target->depth8) {
        target->key32     = k32;
        target->value16   = static_cast<std::int16_t>(value_to_tt(v, ply));
        target->eval16    = static_cast<std::int16_t>(eval);
        target->depth8    = static_cast<std::uint8_t>(depth);
        target->genBound8 = static_cast<std::uint8_t>(generation8_ | b);
    }
}

// ---------------------------------------------------------------------------
// hashfull
// ---------------------------------------------------------------------------
int TranspositionTable::hashfull() const {
    if (clusterCount_ == 0)
        return 0;

    int sampled = 0;
    int current = 0;
    for (std::size_t ci = 0; ci < clusterCount_ && sampled < 1000; ++ci)
        for (int i = 0; i < ClusterSize && sampled < 1000; ++i) {
            const TTEntry& e = table_[ci].entry[i];
            ++sampled;
            if (!e.is_empty()
                && (e.genBound8 & GENERATION_MASK) == generation8_)
                ++current;
        }

    return sampled ? current * 1000 / sampled : 0;
}
