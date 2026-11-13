#include "nnue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
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
              "Grimfang NNUE loader assumes little-endian host layout");
static_assert(offsetof(nnue::Network, feature_weights) == 0);
static_assert(offsetof(nnue::Network, feature_bias) == 786432);
static_assert(offsetof(nnue::Network, output_weights) == 787456);
static_assert(offsetof(nnue::Network, output_bias) == 789504);

namespace embedded_net {
extern const unsigned char g_grimfang_net[];
extern const std::size_t   g_grimfang_net_size;
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
        if (embedded_net::g_grimfang_net_size != nnue::kEmbeddedNetworkBytes)
            fail_network_load("embedded size mismatch");
        const auto* bytes = embedded_net::g_grimfang_net;
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

// --- Accumulator kernels ---------------------------------------------------
// All paths compute in wrapping i16 arithmetic. AVX2 add/sub of i16 wraps in
// two's complement exactly like the scalar static_cast<int16_t> (modular since
// C++20), and addition mod 2^16 is associative/commutative, so the scalar and
// SIMD paths, and incremental vs full refresh, are bit-identical whatever the
// order the weight rows are applied in.
//
// Every pointer is 64-byte aligned: Accumulator is alignas(64) and the
// embedded Network is validated as 64-byte aligned at load, so the AVX2 paths
// use aligned loads/stores.

void add_weights_scalar(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; ++i)
        target[i] = static_cast<std::int16_t>(target[i] + weights[i]);
}

// dst = src + sum(adds) - sum(subs), one pass.
template <int Adds, int Subs>
void fused_update_scalar(std::int16_t* dst, const std::int16_t* src,
                         const std::int16_t* const* adds, const std::int16_t* const* subs) {
    for (int i = 0; i < nnue::kHiddenSize; ++i) {
        int v = src[i];
        for (int a = 0; a < Adds; ++a) v += adds[a][i];
        for (int s = 0; s < Subs; ++s) v -= subs[s][i];
        dst[i] = static_cast<std::int16_t>(v);
    }
}

#if defined(__AVX2__)
inline __m256i load256(const std::int16_t* p) {
    return _mm256_load_si256(reinterpret_cast<const __m256i*>(p));
}

inline void store256(std::int16_t* p, __m256i v) {
    _mm256_store_si256(reinterpret_cast<__m256i*>(p), v);
}

void add_weights_simd(std::int16_t* target, const std::int16_t* weights) {
    for (int i = 0; i < nnue::kHiddenSize; i += 16)
        store256(target + i, _mm256_add_epi16(load256(target + i), load256(weights + i)));
}

template <int Adds, int Subs>
void fused_update_simd(std::int16_t* dst, const std::int16_t* src,
                       const std::int16_t* const* adds, const std::int16_t* const* subs) {
    for (int i = 0; i < nnue::kHiddenSize; i += 16) {
        __m256i v = load256(src + i);
        for (int a = 0; a < Adds; ++a) v = _mm256_add_epi16(v, load256(adds[a] + i));
        for (int s = 0; s < Subs; ++s) v = _mm256_sub_epi16(v, load256(subs[s] + i));
        store256(dst + i, v);
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

template <bool Simd, int Adds, int Subs>
void fused_update(std::int16_t* dst, const std::int16_t* src,
                  const std::int16_t* const* adds, const std::int16_t* const* subs) {
#if defined(__AVX2__)
    if constexpr (Simd) fused_update_simd<Adds, Subs>(dst, src, adds, subs);
    else                fused_update_scalar<Adds, Subs>(dst, src, adds, subs);
#else
    static_assert(!Simd, "SIMD accumulator path requires AVX2");
    fused_update_scalar<Adds, Subs>(dst, src, adds, subs);
#endif
}

const std::int16_t* weight_row(int feature) {
    return network().feature_weights[static_cast<std::size_t>(feature)].vals.data();
}

template <bool Simd>
void apply_update_impl(nnue::AccumulatorPair& child, const nnue::AccumulatorPair& parent,
                       const nnue::AccumulatorUpdate& update) {
    for (int perspective = 0; perspective < COLOR_NB; ++perspective) {
        const auto feature = [perspective](std::uint16_t whiteIdx) {
            return (perspective == WHITE) ? whiteIdx : nnue::black_feature(whiteIdx);
        };
        const std::int16_t* adds[2] = {};
        const std::int16_t* subs[2] = {};
        for (int a = 0; a < update.addCount; ++a)
            adds[a] = weight_row(feature(update.adds[static_cast<std::size_t>(a)]));
        for (int s = 0; s < update.subCount; ++s)
            subs[s] = weight_row(feature(update.subs[static_cast<std::size_t>(s)]));

        const auto p = static_cast<std::size_t>(perspective);
        std::int16_t*       dst = child.byPerspective[p].vals.data();
        const std::int16_t* src = parent.byPerspective[p].vals.data();
        switch (update.addCount * 4 + update.subCount) {
            case 1 * 4 + 1: fused_update<Simd, 1, 1>(dst, src, adds, subs); break; // quiet, promotion
            case 1 * 4 + 2: fused_update<Simd, 1, 2>(dst, src, adds, subs); break; // capture, ep
            case 2 * 4 + 2: fused_update<Simd, 2, 2>(dst, src, adds, subs); break; // castling
            case 0:         fused_update<Simd, 0, 0>(dst, src, adds, subs); break; // no change
            default:        assert(false && "unexpected accumulator update shape"); break;
        }
    }
}

// GRIMFANG_SCALAR_NNUE forces the scalar path even on an AVX2 build, so the
// vectorized inference can be benchmarked head-to-head against the reference.
#if defined(__AVX2__) && !defined(GRIMFANG_SCALAR_NNUE)
constexpr bool kUseSimd = true;
#else
constexpr bool kUseSimd = false;
#endif

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
        __m256i v = load256(accVals + i);
        v = _mm256_max_epi16(v, zero);   // clamp low  -> [0, .]
        v = _mm256_min_epi16(v, qa);     // clamp high -> [0, QA]
        __m256i w = load256(weights + i);
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

template <bool Simd>
void refresh_impl(nnue::AccumulatorPair& acc, const Position& pos) {
    const auto& net = network();
    for (int perspective = 0; perspective < COLOR_NB; ++perspective)
        acc.byPerspective[static_cast<std::size_t>(perspective)] = net.feature_bias;

    Bitboard occ = pos.pieces();
    while (occ) {
        const Square sq = pop_lsb(occ);
        const Piece piece = pos.piece_on(sq);
        for (int perspective = 0; perspective < COLOR_NB; ++perspective) {
            const int idx = feature_index(static_cast<Color>(perspective), piece, sq);
            add_weights<Simd>(acc.byPerspective[static_cast<std::size_t>(perspective)].vals.data(),
                              weight_row(idx));
        }
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
    refresh_impl<kUseSimd>(acc, pos);
}

void nnue::refresh_scalar(AccumulatorPair& acc, const Position& pos) {
    refresh_impl<false>(acc, pos);
}

void nnue::apply_update(AccumulatorPair& child, const AccumulatorPair& parent,
                        const AccumulatorUpdate& update) {
    apply_update_impl<kUseSimd>(child, parent, update);
}

void nnue::apply_update_scalar(AccumulatorPair& child, const AccumulatorPair& parent,
                               const AccumulatorUpdate& update) {
    apply_update_impl<false>(child, parent, update);
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
#if defined(__AVX2__) && !defined(GRIMFANG_SCALAR_NNUE)
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

// ---------------------------------------------------------------------------
// AccumulatorStack
// ---------------------------------------------------------------------------

namespace {

// A refresh costs roughly one weight-row pass per piece per perspective, an
// incremental step about one fused pass, so past this many pending steps a
// full refresh of the live position is the cheaper route to the same result.
// Search trees almost never get near it; long UCI move lists do.
constexpr std::size_t kMaxLazySteps = 16;

} // namespace

nnue::AccumulatorStack::AccumulatorStack(const AccumulatorStack& other)
    : entries_(other.entries_.begin(),
               other.entries_.begin() + static_cast<std::ptrdiff_t>(other.top_ + 1)),
      top_(other.top_) {}

nnue::AccumulatorStack& nnue::AccumulatorStack::operator=(const AccumulatorStack& other) {
    if (this != &other) {
        // Reuse our capacity when large enough; copy only the live entries.
        const auto live = static_cast<std::ptrdiff_t>(other.top_ + 1);
        if (entries_.size() >= static_cast<std::size_t>(live))
            std::copy(other.entries_.begin(), other.entries_.begin() + live, entries_.begin());
        else
            entries_.assign(other.entries_.begin(), other.entries_.begin() + live);
        top_ = other.top_;
    }
    return *this;
}

void nnue::AccumulatorStack::clear() {
    if (entries_.size() < kInitialEntries)
        entries_.resize(kInitialEntries);
    top_ = 0;
    entries_[0].update   = {};
    entries_[0].computed = false;
}

void nnue::AccumulatorStack::reset(const Position& pos) {
    clear();
    refresh(entries_[0].acc, pos);
    entries_[0].computed = true;
}

void nnue::AccumulatorStack::grow() {
    entries_.resize(std::max(kInitialEntries, 2 * entries_.size()));
}

const nnue::AccumulatorPair& nnue::AccumulatorStack::materialise(const Position& pos) {
    // Null-move entries are transparent: evaluate the nearest real ply.
    std::size_t target = top_;
    while (target > 0 && entries_[target].update.empty())
        --target;

    // Walk back to the nearest computed ancestor, counting the real steps.
    std::size_t base  = target;
    std::size_t steps = 0;
    while (!entries_[base].computed && base > 0 && steps <= kMaxLazySteps) {
        if (!entries_[base].update.empty())
            ++steps;
        --base;
    }

    Entry& dst = entries_[target];
    if (!entries_[base].computed) {
        // No computed ancestor within reach (or an unset root): refresh the
        // live position, which has the same pieces as `target` since only
        // null moves lie between them.
        refresh(dst.acc, pos);
        dst.computed = true;
    } else {
        // Apply forward, child written straight from its parent.
        const AccumulatorPair* parent = &entries_[base].acc;
        for (std::size_t i = base + 1; i <= target; ++i) {
            Entry& e = entries_[i];
            if (e.update.empty())
                continue;
            apply_update(e.acc, *parent, e.update);
            e.computed = true;
            parent     = &e.acc;
        }
    }

#if defined(NNUE_INCREMENTAL_CHECK)
    // Debug builds: every lazily materialised accumulator must equal a full
    // refresh of the live position (covers all intermediate steps too, since
    // the result is their sum).
    AccumulatorPair scratch;
    refresh(scratch, pos);
    assert(scratch.byPerspective == dst.acc.byPerspective);
#endif
    return dst.acc;
}
