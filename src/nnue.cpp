#include "nnue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <iostream>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

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

            // The AVX2 SCReLU path forms x*w (x = clamp(acc,0,QA) in [0,QA]) as
            // an i16 via _mm256_mullo_epi16 and relies on it NOT truncating, so
            // that madd(x*w, x) == x*x*w exactly. That holds iff QA*|w| fits an
            // i16. Every validated net so far quantises output weights to
            // [-127,127]; a net that violated this would make the SIMD path
            // diverge from the scalar path, so we fail loudly rather than
            // evaluate silently-wrong positions.
            const std::int32_t scaled =
                static_cast<std::int32_t>(nnue::kQa) * std::abs(static_cast<int>(weight));
            if (scaled > std::numeric_limits<std::int16_t>::max())
                fail_network_load("output weight exceeds AVX2 madd-exact range (QA*|w| > INT16_MAX)");
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

// --- Accumulator +/- weight vector -----------------------------------------
// Both paths compute target[i] = wrap_i16(target[i] +/- weights[i]) over the
// full kHiddenSize lanes. AVX2 add/sub of i16 wraps in two's complement exactly
// like the scalar static_cast<int16_t>, so the two are bit-identical.

void add_weights_scalar(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; ++i)
        target[i] = static_cast<std::int16_t>(target[i] + weights[i]);
}

void sub_weights_scalar(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; ++i)
        target[i] = static_cast<std::int16_t>(target[i] - weights[i]);
}

#if defined(__AVX2__)
void add_weights_simd(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; i += 16) {
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + i));
        __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(target + i), _mm256_add_epi16(t, w));
    }
}

void sub_weights_simd(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; i += 16) {
        __m256i t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + i));
        __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(target + i), _mm256_sub_epi16(t, w));
    }
}
#endif

template <bool Simd>
void add_weights(std::int16_t* target, const std::int16_t* weights) {
#if defined(__AVX2__)
    if constexpr (Simd) add_weights_simd(target, weights);
    else                add_weights_scalar(target, weights);
#else
    static_assert(!Simd, "SIMD accumulator path requires AVX2");
    add_weights_scalar(target, weights);
#endif
}

template <bool Simd>
void sub_weights(std::int16_t* target, const std::int16_t* weights) {
#if defined(__AVX2__)
    if constexpr (Simd) sub_weights_simd(target, weights);
    else                sub_weights_scalar(target, weights);
#else
    static_assert(!Simd, "SIMD accumulator path requires AVX2");
    sub_weights_scalar(target, weights);
#endif
}

template <bool Simd>
void apply_delta_impl(nnue::AccumulatorPair& acc, const nnue::FeatureDelta& delta) {
    const auto& net = network();
    for (int perspective = 0; perspective < COLOR_NB; ++perspective) {
        const int idx = feature_index(static_cast<Color>(perspective), delta.piece, delta.square);
        const std::int16_t* weights = net.feature_weights[static_cast<std::size_t>(idx)].vals.data();
        std::int16_t* target = acc.byPerspective[static_cast<std::size_t>(perspective)].vals.data();
        if (delta.add) add_weights<Simd>(target, weights);
        else           sub_weights<Simd>(target, weights);
    }
}

// STOCKWOLF_SCALAR_NNUE forces the scalar path even on an AVX2 build, so the
// vectorized inference can be benchmarked head-to-head against the reference.
#if defined(__AVX2__) && !defined(STOCKWOLF_SCALAR_NNUE)
constexpr bool kUseSimd = true;
#else
constexpr bool kUseSimd = false;
#endif

void apply_delta(nnue::AccumulatorPair& acc, const nnue::FeatureDelta& delta) {
    apply_delta_impl<kUseSimd>(acc, delta);
}

void apply_delta_scalar(nnue::AccumulatorPair& acc, const nnue::FeatureDelta& delta) {
    apply_delta_impl<false>(acc, delta);
}

// --- SCReLU dot product -----------------------------------------------------

#if defined(__AVX2__)
inline std::int32_t hsum_epi32(__m256i v) {
    __m128i lo  = _mm256_castsi256_si128(v);
    __m128i hi  = _mm256_extracti128_si256(v, 1);
    __m128i sum = _mm_add_epi32(lo, hi);
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(sum);
}

// Accumulate sum_i clamp(acc[i],0,QA)^2 * w[i] into 8 i32 lanes.
inline __m256i screlu_accumulate(__m256i acc32, const std::int16_t* accVals,
                                 const std::int16_t* weights) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i qa   = _mm256_set1_epi16(static_cast<short>(nnue::kQa));
    for (int i = 0; i < nnue::kHiddenSize; i += 16) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(accVals + i));
        v = _mm256_max_epi16(v, zero);   // clamp low  -> [0, .]
        v = _mm256_min_epi16(v, qa);     // clamp high -> [0, QA]
        __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        // |v*w| <= QA*127 < INT16_MAX, so mullo keeps the exact product.
        __m256i weighted = _mm256_mullo_epi16(v, w);
        // madd forms (v*w)*v = v*v*w per lane (fits i32) and pairwise-sums.
        acc32 = _mm256_add_epi32(acc32, _mm256_madd_epi16(weighted, v));
    }
    return acc32;
}
#endif

} // namespace

namespace {

template <void (*Apply)(nnue::AccumulatorPair&, const nnue::FeatureDelta&)>
void refresh_impl(nnue::AccumulatorPair& acc, const Position& pos) {
    const auto& net = network();
    for (int perspective = 0; perspective < COLOR_NB; ++perspective)
        acc.byPerspective[static_cast<std::size_t>(perspective)] = net.feature_bias;

    Bitboard occ = pos.pieces();
    while (occ) {
        const Square sq = pop_lsb(occ);
        const Piece piece = pos.piece_on(sq);
        nnue::FeatureDelta delta { piece, sq, true };
        Apply(acc, delta);
    }
}

// Final dequant, identical to the scalar reference. `sum` is bit-identical
// between the scalar and SIMD dot products, so this yields the same Value.
inline Value dequant(std::int32_t sum, std::int16_t output_bias) {
    const std::int32_t eval =
        (sum / nnue::kQa + output_bias) * nnue::kEvalScale / (nnue::kQa * nnue::kQb);
    return static_cast<Value>(eval);
}

} // namespace

void nnue::refresh(AccumulatorPair& acc, const Position& pos) {
    refresh_impl<apply_delta>(acc, pos);
}

void nnue::refresh_scalar(AccumulatorPair& acc, const Position& pos) {
    refresh_impl<apply_delta_scalar>(acc, pos);
}

void nnue::update(AccumulatorPair& acc, std::span<const FeatureDelta> deltas) {
    for (const FeatureDelta& delta : deltas)
        apply_delta(acc, delta);
}

Value nnue::evaluate_scalar(const Position& pos) {
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

    return dequant(sum, net.output_bias);
}

Value nnue::evaluate(const Position& pos) {
#if defined(__AVX2__) && !defined(STOCKWOLF_SCALAR_NNUE)
    const auto& net = network();
    const auto& acc = pos.nnue_accumulators();
    const Color stm = pos.side_to_move();
    const std::int16_t* us   = acc.byPerspective[static_cast<std::size_t>(stm)].vals.data();
    const std::int16_t* them = acc.byPerspective[static_cast<std::size_t>(~stm)].vals.data();

    __m256i acc32 = _mm256_setzero_si256();
    acc32 = screlu_accumulate(acc32, us,   &net.output_weights[0]);
    acc32 = screlu_accumulate(acc32, them, &net.output_weights[kHiddenSize]);
    const std::int32_t sum = hsum_epi32(acc32);

    return dequant(sum, net.output_bias);
#else
    return evaluate_scalar(pos);
#endif
}
