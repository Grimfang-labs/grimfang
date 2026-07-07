#include "nnue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <iostream>

#include "bitboard.hpp"
#include "position.hpp"

static_assert(std::endian::native == std::endian::little,
              "StockWolf NNUE loader assumes little-endian host layout");
static_assert(offsetof(nnue::Network, feature_weights) == 0);
static_assert(offsetof(nnue::Network, feature_bias) == 786432);
static_assert(offsetof(nnue::Network, output_weights) == 787456);
static_assert(offsetof(nnue::Network, output_bias) == 789504);

namespace embedded_net {
extern const unsigned char g_stockwolf_net_001[];
extern const std::size_t   g_stockwolf_net_001_size;
} // namespace embedded_net

namespace {

[[noreturn]] void fail_network_load(const char* message) {
    std::cerr << "NNUE network load failure: " << message << '\n';
    std::abort();
}

int feature_index(Color perspective, Piece piece, Square sq) {
    const Color ownColor = (perspective == WHITE) ? WHITE : BLACK;
    const int colorBase = (color_of(piece) == ownColor) ? 0 : 384;
    const int square = (perspective == WHITE) ? static_cast<int>(sq)
                                              : (static_cast<int>(sq) ^ 56);
    return colorBase + 64 * static_cast<int>(type_of(piece)) + square;
}

const nnue::Network& network() {
    static const nnue::Network* net = []() -> const nnue::Network* {
        if (embedded_net::g_stockwolf_net_001_size != nnue::kEmbeddedNetworkBytes)
            fail_network_load("embedded size mismatch");
        const auto* bytes = embedded_net::g_stockwolf_net_001;
        if ((reinterpret_cast<std::uintptr_t>(bytes) % alignof(nnue::Network)) != 0)
            fail_network_load("embedded bytes are not 64-byte aligned");
        const auto* loaded = reinterpret_cast<const nnue::Network*>(bytes);

        std::int64_t positiveWeightSum = 0;
        std::int64_t negativeWeightSum = 0;
        for (std::int16_t weight : loaded->output_weights) {
            if (weight > 0) positiveWeightSum += weight;
            if (weight < 0) negativeWeightSum -= weight;
        }

        const std::int64_t maxActivation = static_cast<std::int64_t>(nnue::kQa) * nnue::kQa;
        const std::int64_t maxPositiveSum = maxActivation * positiveWeightSum;
        const std::int64_t maxNegativeSum = maxActivation * negativeWeightSum;
        if (maxPositiveSum > std::numeric_limits<std::int32_t>::max()
            || maxNegativeSum > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1)
            fail_network_load("output layer exceeds validated int32 accumulation range");

        return loaded;
    }();
    return *net;
}

void apply_delta(nnue::AccumulatorPair& acc, const nnue::FeatureDelta& delta) {
    const auto& net = network();
    for (int perspective = 0; perspective < COLOR_NB; ++perspective) {
        const int idx = feature_index(static_cast<Color>(perspective), delta.piece, delta.square);
        const auto& weights = net.feature_weights[static_cast<std::size_t>(idx)].vals;
        auto& target = acc.byPerspective[static_cast<std::size_t>(perspective)].vals;
        for (int i = 0; i < nnue::kHiddenSize; ++i) {
            const int value = delta.add ? target[i] + weights[i] : target[i] - weights[i];
            target[i] = static_cast<std::int16_t>(value);
        }
    }
}

} // namespace

void nnue::refresh(AccumulatorPair& acc, const Position& pos) {
    const auto& net = network();
    for (int perspective = 0; perspective < COLOR_NB; ++perspective)
        acc.byPerspective[static_cast<std::size_t>(perspective)] = net.feature_bias;

    Bitboard occ = pos.pieces();
    while (occ) {
        const Square sq = pop_lsb(occ);
        const Piece piece = pos.piece_on(sq);
        FeatureDelta delta { piece, sq, true };
        apply_delta(acc, delta);
    }
}

void nnue::update(AccumulatorPair& acc, std::span<const FeatureDelta> deltas) {
    for (const FeatureDelta& delta : deltas)
        apply_delta(acc, delta);
}

Value nnue::evaluate(const Position& pos) {
    const auto& net = network();
    const auto& acc = pos.nnue_accumulators();
    const Color stm = pos.side_to_move();
    const auto& us = acc.byPerspective[static_cast<std::size_t>(stm)].vals;
    const auto& them = acc.byPerspective[static_cast<std::size_t>(~stm)].vals;

    std::int32_t sum = 0;
    // Each a*a*w term fits i32, and the embedded net is validated at load time
    // so the full 1024-term signed accumulation range also stays within i32.
    for (int i = 0; i < kHiddenSize; ++i) {
        const std::int32_t a = std::clamp<std::int32_t>(us[i], 0, kQa);
        const std::int32_t b = std::clamp<std::int32_t>(them[i], 0, kQa);
        sum += a * a * net.output_weights[static_cast<std::size_t>(i)];
        sum += b * b * net.output_weights[static_cast<std::size_t>(kHiddenSize + i)];
    }

    const std::int32_t eval =
        (sum / kQa + net.output_bias) * kEvalScale / (kQa * kQb);
    return static_cast<Value>(eval);
}
