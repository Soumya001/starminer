/**
 * h160_bloom_filter.cu — GPU-side Bloom filter probe for 20-byte HASH160.
 *
 * Implements bloom_check_h160 using MurmurHash3 with the Kirsch-Mitzenmacher
 * double-hashing optimization: h_i = (h1 + i*h2) mod num_bits.
 *
 * The hash and bit layout must match the host-side bloom builder
 * (src/tools/build_bloom.cpp / utxo_bloom_builder.hpp) exactly so that
 * GPU probes read the same .blf files the builder writes.
 */

#include <cstdint>
#include <cuda_runtime.h>

namespace starminer {
namespace gpu {

// ---------------------------------------------------------------------------
// MurmurHash3 — 32-bit output, tuned for exactly 20 input bytes (HASH160).
// ---------------------------------------------------------------------------

__device__ static uint32_t murmur3_20(const uint8_t* data, uint32_t seed)
{
    constexpr uint32_t c1 = 0xcc9e2d51u;
    constexpr uint32_t c2 = 0x1b873593u;

    uint32_t h1 = seed;

    // Five 4-byte little-endian blocks.
    #pragma unroll
    for (int i = 0; i < 5; ++i) {
        uint32_t k1;
        k1  = (uint32_t)data[i*4 + 0];
        k1 |= (uint32_t)data[i*4 + 1] << 8;
        k1 |= (uint32_t)data[i*4 + 2] << 16;
        k1 |= (uint32_t)data[i*4 + 3] << 24;

        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;

        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5u + 0xe6546b64u;
    }

    // Finalization — length = 20.
    h1 ^= 20u;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;
    return h1;
}

// ---------------------------------------------------------------------------
// bloom_check_h160 — probe `h160[20]` against the bloom filter.
//
// Parameters match src/gpu/v2/brain_wallet_v2.cu's forward declaration and
// the host-side bloom reader in pool_solver.cpp exactly.
// ---------------------------------------------------------------------------

__device__ bool bloom_check_h160(
    const uint8_t* bloom_data,
    uint64_t       num_bits,
    uint32_t       num_hashes,
    uint32_t       seed,
    const uint8_t* h160)
{
    // Kirsch-Mitzenmacher: two independent hashes, combine for each slot.
    const uint32_t h1 = murmur3_20(h160, seed);
    const uint32_t h2 = murmur3_20(h160, seed ^ 0x5f3759dfu);

    uint64_t running = (uint64_t)h1;
    const uint64_t step    = (uint64_t)h2;

    #pragma unroll 4
    for (uint32_t i = 0; i < num_hashes; ++i) {
        const uint64_t bit_idx  = running % num_bits;
        const uint64_t byte_idx = bit_idx >> 3;
        const uint32_t bit_mask = 1u << (uint32_t)(bit_idx & 7u);
        if (!(bloom_data[byte_idx] & bit_mask)) return false;
        running += step;
    }
    return true;
}

}  // namespace gpu
}  // namespace starminer
