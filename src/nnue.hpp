#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "score.hpp"
#include "types.hpp"

class Position;

namespace nnue {

constexpr int kInputFeatures = 768;
constexpr int kHiddenSize = 512;
constexpr int kQa = 255;
constexpr int kQb = 64;
constexpr int kEvalScale = 400;
constexpr std::size_t kEmbeddedNetworkBytes = 789568;

struct alignas(64) Accumulator {
    std::array<std::int16_t, kHiddenSize> vals {};

    bool operator==(const Accumulator&) const = default;
};

struct AccumulatorPair {
    std::array<Accumulator, COLOR_NB> byPerspective {};

    bool operator==(const AccumulatorPair&) const = default;
};

struct Network {
    std::array<Accumulator, kInputFeatures> feature_weights;
    Accumulator                             feature_bias;
    std::array<std::int16_t, 2 * kHiddenSize> output_weights;
    std::int16_t                            output_bias;
};

static_assert(sizeof(Accumulator) == 1024);
static_assert(alignof(Accumulator) == 64);
static_assert(alignof(Network) == 64);
static_assert(sizeof(Network) == kEmbeddedNetworkBytes);

// Input feature of (piece, square) from WHITE's perspective:
// colorBase (0 own / 384 enemy) + 64 * pieceType + square. The BLACK-perspective
// index of the same piece is derived with black_feature(), so a move records
// one index per changed feature rather than one per perspective.
constexpr std::uint16_t white_feature(Piece pc, Square sq) {
    return static_cast<std::uint16_t>((color_of(pc) == WHITE ? 0 : 384)
                                      + 64 * static_cast<int>(type_of(pc))
                                      + static_cast<int>(sq));
}

// Swap own/enemy halves and mirror the square vertically (sq ^ 56). The
// within-half offset 64*pt+sq is < 384, so mirroring never crosses halves.
constexpr std::uint16_t black_feature(std::uint16_t whiteIdx) {
    const int swapped = (whiteIdx >= 384) ? whiteIdx - 384 : whiteIdx + 384;
    return static_cast<std::uint16_t>(swapped ^ 56);
}

// Feature changes made by one move (child = parent + adds - subs). Every legal
// move needs at most 2 adds and 2 subs: quiet 1/1, capture or en passant 1/2,
// promotion 1/1, capture-promotion 1/2, castling 2/2. A null move records
// nothing (0/0): the child's accumulators are identical to the parent's.
struct AccumulatorUpdate {
    std::array<std::uint16_t, 2> adds {};
    std::array<std::uint16_t, 2> subs {};
    std::uint8_t                 addCount = 0;
    std::uint8_t                 subCount = 0;

    void add(Piece pc, Square sq) { adds[addCount++] = white_feature(pc, sq); }
    void sub(Piece pc, Square sq) { subs[subCount++] = white_feature(pc, sq); }
    bool empty() const { return addCount == 0 && subCount == 0; }
};

void refresh(AccumulatorPair& acc, const Position& pos);
Value evaluate(const Position& pos);

// child = parent + update, in one fused pass per perspective (child and parent
// must not alias).
void apply_update(AccumulatorPair& child, const AccumulatorPair& parent,
                  const AccumulatorUpdate& update);

// Retained scalar reference paths. `evaluate`/`refresh`/`apply_update` above use
// AVX2 when available (see nnue.cpp); these always compute with the scalar
// integer math so tests can assert the vectorized path is bit-identical.
Value evaluate_scalar(const Position& pos);
void refresh_scalar(AccumulatorPair& acc, const Position& pos);
void apply_update_scalar(AccumulatorPair& child, const AccumulatorPair& parent,
                         const AccumulatorUpdate& update);

// ---------------------------------------------------------------------------
// AccumulatorStack - one entry per ply since the root (set_fen), updated
// lazily.
//
// make_move only records the move's feature changes in a new entry (push());
// nothing is computed. When an eval asks for the current accumulators,
// current() walks back to the nearest computed ancestor and applies the
// recorded updates forward, writing each child directly from its parent, so
// every ancestor on the path becomes computed for its siblings too. Nodes
// whose eval is never requested (in-check nodes, TT cutoffs, perft) cost only
// the bookkeeping. Null-move entries hold an empty update and are never
// computed: they resolve to their nearest non-null ancestor's accumulators
// (byPerspective is indexed by absolute colour, so a pass needs no swap).
// unmake/undo_null just pop().
//
// Materialisation is only ever for the CURRENT ply, so a full refresh of the
// live position is always a valid (bit-identical) fallback, used when the walk
// back is long (e.g. after a long `position ... moves ...` list).
//
// The storage grows by doubling (never per move) and is preallocated for a
// fresh search tree; copies duplicate only the live entries.
// ---------------------------------------------------------------------------
class AccumulatorStack {
public:
    AccumulatorStack() { clear(); }
    AccumulatorStack(const AccumulatorStack& other);
    AccumulatorStack& operator=(const AccumulatorStack& other);
    AccumulatorStack(AccumulatorStack&&) noexcept = default;
    AccumulatorStack& operator=(AccumulatorStack&&) noexcept = default;
    ~AccumulatorStack() = default;

    // Drop every ply and leave a single, not-yet-computed root entry.
    void clear();

    // Root = full refresh of `pos` (called once the position is set up).
    void reset(const Position& pos);

    // Start a new ply; the caller fills in the returned (empty) update.
    AccumulatorUpdate& push() {
        if (top_ + 1 == entries_.size())
            grow();
        Entry& e   = entries_[++top_];
        e.update   = {};
        e.computed = false;
        return e.update;
    }

    void push_null() { push(); }
    void pop() { --top_; }

    // Accumulators of the current ply (`pos` must be the live position this
    // stack belongs to), materialised on demand.
    const AccumulatorPair& current(const Position& pos) {
        const Entry& e = entries_[top_];
        return e.computed ? e.acc : materialise(pos);
    }

private:
    struct Entry {
        AccumulatorPair   acc;               // valid only when `computed`
        AccumulatorUpdate update;            // change from the previous ply
        bool              computed = false;  // never set on null-move entries
    };

    // Preallocated entries: a full-depth search from a fresh root never grows.
    static constexpr std::size_t kInitialEntries = 2 * MAX_PLY;

    const AccumulatorPair& materialise(const Position& pos);
    void grow();

    std::vector<Entry> entries_;   // [0, top_] are live; entries_[0] is the root
    std::size_t        top_ = 0;
};

} // namespace nnue
