#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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

struct FeatureDelta {
    Piece  piece;
    Square square;
    bool   add;
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

void refresh(AccumulatorPair& acc, const Position& pos);
void update(AccumulatorPair& acc, std::span<const FeatureDelta> deltas);
Value evaluate(const Position& pos);

// Retained scalar reference paths. `evaluate`/`refresh` above use AVX2 when
// available (see nnue.cpp); these always compute with the scalar integer math
// so tests can assert the vectorized path is bit-identical.
Value evaluate_scalar(const Position& pos);
void refresh_scalar(AccumulatorPair& acc, const Position& pos);

} // namespace nnue
