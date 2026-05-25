/**
 * Brain Wallet v2 GPU Pipeline — Item 6 implementation
 *
 * This file ships the FIRST real kernel: puzzle-mode.  It evaluates a
 * passphrase against a fixed list of "puzzle target" private keys, with
 * each target carrying a (mask, value) pair.  The kernel can short-circuit
 * before EC_MUL when puzzle-mode is the only enabled mode, which makes it
 * an order of magnitude faster than the standard brain-wallet pipeline.
 *
 * Items 1, 2, 4, 5 still return cudaErrorNotSupported -- their kernels
 * land in follow-up commits per docs/BRAINWALLET-V2-SPEC.md.
 *
 * The SHA-256 helpers are intentionally duplicated here (rather than
 * refactored out of fused_pipeline.cu) so this kernel is self-contained:
 *   - no risk of breaking the production fused pipeline
 *   - the v2 file can be excluded/included in the build independently
 *   - simpler diff to review
 */

#include "brain_wallet_v2.hpp"
#include "device_hashes.cuh"   // device::sha512 + hmac_sha512 + pbkdf2 + bip32
#include "../secp256k1_device_api.cuh"   // uint256, ECPointAffine, ec_mul_simple
#include <cuda_runtime.h>
#include <cstring>
#include <cstdio>
#include <type_traits>

namespace starminer {
namespace gpu {
namespace v2 {

// ===========================================================================
// SHA-256 is provided by device_hashes.cuh (device::sha256). The legacy
// in-file v2_sha256 + SHA256_K_V2 were removed in PR #15 review (Gemini):
// duplication wasted constant memory in the unity build and risked drift
// vs the shared header.
// ===========================================================================

// Convenience alias so the existing call sites stay readable.
static __device__ __forceinline__ void v2_sha256(
    const uint8_t* msg, size_t len, uint8_t out[32])
{
    device::sha256(msg, (uint32_t)len, out);
}

// ===========================================================================
// Constant-memory storage for puzzle targets (item 6)
// ===========================================================================

__device__ __constant__ PuzzleTarget c_puzzle_targets[PUZZLE_TARGET_MAX];
__device__ __constant__ int          c_puzzle_target_count;

// ===========================================================================
// Helpers for derivation schemes used by the puzzle-mode kernel.
//
// Each helper takes:
//   - the passphrase bytes
//   - puzzle_n N (so the function can encode N into the hash input)
//   - 32-byte output buffer (big-endian SHA-256 result)
//
// Scheme parity must match the host-side reference in
// tools/brainwallet/puzzle_mode.py (functions _s_*).
// ===========================================================================

// ---------------------------------------------------------------------------
// DerivationScheme enum implementations (Phase 3, v1.4.0).
// Each function takes the passphrase and writes a 32-byte priv into `out`.
// SHA-256 schemes use the in-file v2_sha256; SHA-512 schemes use the
// shared device::sha512 / device::hmac_sha512 from device_hashes.cuh.
// ---------------------------------------------------------------------------

// DerivationScheme::SHA256_PW                  priv = SHA256(pw)
static __device__ void v2_d_sha256_pw(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    v2_sha256(pw, pw_len, out);
}

// DerivationScheme::SHA256_SHA256_PW          priv = SHA256(SHA256(pw))
static __device__ void v2_d_sha256_sha256_pw(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    uint8_t inner[32];
    v2_sha256(pw, pw_len, inner);
    v2_sha256(inner, 32, out);
}

// DerivationScheme::SHA256_PW_NEWLINE         priv = SHA256(pw || 0x0a)
static __device__ void v2_d_sha256_pw_newline(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    if (pw_len > 255) pw_len = 255;
    uint8_t buf[256];
    for (size_t i = 0; i < pw_len; ++i) buf[i] = pw[i];
    buf[pw_len] = 0x0a;
    v2_sha256(buf, pw_len + 1, out);
}

// DerivationScheme::SHA256_PW_PW              priv = SHA256(pw || pw)
static __device__ void v2_d_sha256_pw_pw(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    if (pw_len > 128) pw_len = 128;
    uint8_t buf[256];
    for (size_t i = 0; i < pw_len; ++i) buf[i] = pw[i];
    for (size_t i = 0; i < pw_len; ++i) buf[pw_len + i] = pw[i];
    v2_sha256(buf, pw_len * 2, out);
}

// DerivationScheme::SHA256_SHA256_PW_PW       priv = SHA256(SHA256(pw) || pw)
static __device__ void v2_d_sha256_sha256_pw_pw(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    // Cap at 128 to match the Metal port's buffer budget (combo[32+128]).
    if (pw_len > 128) pw_len = 128;
    uint8_t buf[256];
    uint8_t inner[32];
    v2_sha256(pw, pw_len, inner);
    for (int i = 0; i < 32; ++i) buf[i] = inner[i];
    for (size_t i = 0; i < pw_len; ++i) buf[32 + i] = pw[i];
    v2_sha256(buf, 32 + pw_len, out);
}

// DerivationScheme::SHA256_ITER_16            priv = SHA256^16(pw)
static __device__ void v2_d_sha256_iter_16(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    uint8_t a[32];
    v2_sha256(pw, pw_len, a);
    #pragma unroll
    for (int i = 0; i < 15; ++i) {
        v2_sha256(a, 32, a);
    }
    for (int i = 0; i < 32; ++i) out[i] = a[i];
}

// DerivationScheme::HMAC_SHA512_PW            priv = HMAC-SHA512(pw, "")[:32]
// (Empty key, passphrase as message; matches the legacy "HMAC stretch" pattern
// in some early Java/Android brain-wallet generators.)
static __device__ void v2_d_hmac_sha512_pw(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    uint8_t mac[64];
    static const uint8_t empty_key[1] = {0};
    device::hmac_sha512(empty_key, 0, pw, (uint32_t)pw_len, mac);
    for (int i = 0; i < 32; ++i) out[i] = mac[i];
}

// DerivationScheme::SHA512_PW_HALF            priv = SHA512(pw)[:32]
static __device__ void v2_d_sha512_pw_half(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    uint8_t full[64];
    device::sha512(pw, (uint32_t)pw_len, full);
    for (int i = 0; i < 32; ++i) out[i] = full[i];
}

// (The legacy per-puzzle-N salted scheme `v2_scheme_S2` and its kernel
// `v2_puzzle_only_kernel_S2` were removed in PR #15 review (Gemini): the
// kernel was unreachable from the public API after the Phase 3 multi-scheme
// refactor and reported its hits as SHA256_PW, which would have misled the
// host. The CPU prototype in tools/brainwallet/puzzle_mode.py retains the
// algorithm if anyone ever wants to reintroduce it.)

// ===========================================================================
// Pack a 32-byte big-endian hash into 4 little-endian-by-limb uint64s.
//   limb[0] = bits 0..63   (= hash_be[24..31])
//   limb[1] = bits 64..127  (= hash_be[16..23])
//   limb[2] = bits 128..191 (= hash_be[ 8..15])
//   limb[3] = bits 192..255 (= hash_be[ 0.. 7])
// ===========================================================================
static __device__ __forceinline__ void v2_hash_to_limbs(
    const uint8_t hash_be[32], uint64_t out_limbs[4])
{
    auto pack_be64 = [&](const uint8_t* p) -> uint64_t {
        uint64_t v = 0;
        #pragma unroll
        for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)p[i];
        return v;
    };
    out_limbs[0] = pack_be64(hash_be + 24);
    out_limbs[1] = pack_be64(hash_be + 16);
    out_limbs[2] = pack_be64(hash_be +  8);
    out_limbs[3] = pack_be64(hash_be +  0);
}

// ---------------------------------------------------------------------------
// Multi-scheme dispatch kernel (Phase 3, v1.4.0).
//
// Templated on a single DerivationScheme so the compiler unrolls the inner
// hash composition. The host calls this kernel once per scheme bit set in
// scheme_mask; each call exercises ALL puzzle targets against THAT scheme's
// derivation. Dynamic dispatch on the device is avoided -- separate kernel
// launches are cheap and keep register pressure down.
// ---------------------------------------------------------------------------

template <DerivationScheme S>
static __device__ __forceinline__ void v2_derive(
    const uint8_t* pw, size_t pw_len, uint8_t out[32])
{
    if      constexpr (S == DerivationScheme::SHA256_PW)
        v2_d_sha256_pw(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA256_SHA256_PW)
        v2_d_sha256_sha256_pw(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA256_PW_NEWLINE)
        v2_d_sha256_pw_newline(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA256_PW_PW)
        v2_d_sha256_pw_pw(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA256_SHA256_PW_PW)
        v2_d_sha256_sha256_pw_pw(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA256_ITER_16)
        v2_d_sha256_iter_16(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::HMAC_SHA512_PW)
        v2_d_hmac_sha512_pw(pw, pw_len, out);
    else if constexpr (S == DerivationScheme::SHA512_PW_HALF)
        v2_d_sha512_pw_half(pw, pw_len, out);
    else
        v2_d_sha256_pw(pw, pw_len, out);  // fallback for unknown values
}

// __launch_bounds__(256, 4): hint the compiler that we launch with up
// to 256 threads/block and want at least 4 resident blocks/SM. Without
// this hint nvcc over-allocates registers for the S7/S8 (SHA-512 +
// HMAC) instantiations and occupancy collapses on Ampere+. (Gemini PR
// #15 MED finding.)
template <DerivationScheme S>
__global__ __launch_bounds__(256, 4) void v2_puzzle_only_kernel_scheme(
    const uint8_t* __restrict__ passphrases,
    const uint32_t* __restrict__ offsets,
    const uint32_t* __restrict__ lengths,
    size_t count,
    V2MatchRecord* __restrict__ matches,
    uint32_t* __restrict__ match_count)
{
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    const uint8_t* pw = passphrases + offsets[idx];
    uint32_t pw_len = lengths[idx];

    uint8_t hash_out[32];
    v2_derive<S>(pw, pw_len, hash_out);

    uint64_t hash_limbs[4];
    v2_hash_to_limbs(hash_out, hash_limbs);

    const int n_targets = c_puzzle_target_count;
    for (int ti = 0; ti < n_targets; ++ti) {
        const PuzzleTarget& t = c_puzzle_targets[ti];
        bool match = true;
        #pragma unroll
        for (int j = 0; j < 4; ++j) {
            if ((hash_limbs[j] & t.low_mask[j]) != t.low_value[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            uint32_t slot = atomicAdd(match_count, 1u);
            if (slot < V2_MAX_MATCHES_PER_BATCH) {
                V2MatchRecord rec{};
                rec.pp_idx    = (uint32_t)idx;
                rec.puzzle_n  = t.puzzle_n;
                rec.scheme_id = (uint8_t)S;
                rec.kind      = (uint8_t)V2MatchRecord::Kind::PUZZLE_KEY_HIT;
                matches[slot] = rec;
            }
        }
    }
}

// ===========================================================================
// Public API
// ===========================================================================

// ---------------------------------------------------------------------------
// Phase 4: fused derivation + EC multiply + multi-address bloom kernel.
//
// Each thread: derives a private key from its passphrase, calls ec_mul_simple
// to get the public key, then checks each enabled address type against bloom.
// ---------------------------------------------------------------------------

template <DerivationScheme S>
__global__ __launch_bounds__(128, 2) void v2_addr_kernel(
    const uint8_t*  __restrict__ passphrases,
    const uint32_t* __restrict__ offsets,
    const uint32_t* __restrict__ lengths,
    size_t count,
    uint32_t addr_mask,
    const uint8_t*  __restrict__ bloom,
    uint64_t bloom_bits,
    int bloom_hashes,
    uint32_t bloom_seed,
    V2MatchRecord*  __restrict__ matches,
    uint32_t*       __restrict__ match_count)
{
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    const uint8_t* pw = passphrases + offsets[idx];
    uint32_t pw_len = lengths[idx];

    // Derive private key
    uint8_t priv[32];
    v2_derive<S>(pw, pw_len, priv);

    // EC multiply: priv → (pub_x, pub_y)
    uint256 scalar;
    #pragma unroll
    for (int i = 0; i < 8; ++i) {
        uint32_t w = 0;
        int base = (7 - i) * 4;
        w |= (uint32_t)priv[base]     << 24;
        w |= (uint32_t)priv[base + 1] << 16;
        w |= (uint32_t)priv[base + 2] <<  8;
        w |= (uint32_t)priv[base + 3];
        scalar.limbs[i] = w;
    }
    ECPointAffine P;
    ec_mul_simple(P, scalar);

    // Convert to big-endian byte arrays for address derivation
    uint8_t pub_x[32], pub_y[32];
    #pragma unroll
    for (int i = 0; i < 8; ++i) {
        uint32_t lx = P.x.limbs[7 - i], ly = P.y.limbs[7 - i];
        pub_x[i*4+0] = (uint8_t)(lx >> 24); pub_x[i*4+1] = (uint8_t)(lx >> 16);
        pub_x[i*4+2] = (uint8_t)(lx >>  8); pub_x[i*4+3] = (uint8_t)(lx      );
        pub_y[i*4+0] = (uint8_t)(ly >> 24); pub_y[i*4+1] = (uint8_t)(ly >> 16);
        pub_y[i*4+2] = (uint8_t)(ly >>  8); pub_y[i*4+3] = (uint8_t)(ly      );
    }

    // Bloom probe helper
    auto check_and_emit = [&](uint8_t addr_type, const uint8_t h160[20]) __device__ {
        if (!bloom || bloom_bits == 0) return;
        if (!::starminer::gpu::bloom_check_h160(bloom, bloom_bits, (uint32_t)bloom_hashes, bloom_seed, h160))
            return;
        uint32_t slot = atomicAdd(match_count, 1u);
        if (slot < V2_MAX_MATCHES_PER_BATCH) {
            V2MatchRecord rec{};
            rec.pp_idx    = (uint32_t)idx;
            rec.addr_type = addr_type;
            rec.scheme_id = (uint8_t)S;
            rec.kind      = (uint8_t)V2MatchRecord::Kind::ADDR_BLOOM_HIT;
            matches[slot] = rec;
        }
    };

    if (addr_mask & addr_bit(AddressType::P2PKH_UNCOMPRESSED)) {
        uint8_t buf[65], h[20];
        buf[0] = 0x04;
        for (int i = 0; i < 32; ++i) { buf[1+i] = pub_x[i]; buf[33+i] = pub_y[i]; }
        device::hash160(buf, 65u, h);
        check_and_emit((uint8_t)AddressType::P2PKH_UNCOMPRESSED, h);
    }
    if (addr_mask & addr_bit(AddressType::P2PKH_COMPRESSED)) {
        uint8_t comp[33], h[20];
        comp[0] = (pub_y[31] & 1u) ? 0x03 : 0x02;
        for (int i = 0; i < 32; ++i) comp[1+i] = pub_x[i];
        device::hash160(comp, 33u, h);
        check_and_emit((uint8_t)AddressType::P2PKH_COMPRESSED, h);
    }
    if (addr_mask & addr_bit(AddressType::P2WPKH_V0)) {
        uint8_t comp[33], h[20];
        comp[0] = (pub_y[31] & 1u) ? 0x03 : 0x02;
        for (int i = 0; i < 32; ++i) comp[1+i] = pub_x[i];
        device::hash160(comp, 33u, h);
        check_and_emit((uint8_t)AddressType::P2WPKH_V0, h);
    }
    if (addr_mask & addr_bit(AddressType::P2SH_P2WPKH)) {
        uint8_t comp[33], inner[20], redeem[22], h[20];
        comp[0] = (pub_y[31] & 1u) ? 0x03 : 0x02;
        for (int i = 0; i < 32; ++i) comp[1+i] = pub_x[i];
        device::hash160(comp, 33u, inner);
        redeem[0] = 0x00; redeem[1] = 0x14;
        for (int i = 0; i < 20; ++i) redeem[2+i] = inner[i];
        device::hash160(redeem, 22u, h);
        check_and_emit((uint8_t)AddressType::P2SH_P2WPKH, h);
    }
}

// Forward-declare the bloom_check_h160 from h160_bloom_filter.cu (same
// namespace; accessible via separable compilation).
namespace starminer { namespace gpu {
extern __device__ bool bloom_check_h160(
    const uint8_t* bloom_data,
    uint64_t num_bits,
    uint32_t num_hashes,
    uint32_t seed,
    const uint8_t* h160);
}}

static cudaError_t v2_brain_wallet_addr_batch(
    const uint8_t*  d_passphrases,
    const uint32_t* d_offsets,
    const uint32_t* d_lengths,
    size_t count,
    uint32_t scheme_mask,
    uint32_t addr_mask,
    const uint8_t*  d_bloom,
    uint64_t bloom_bits,
    int bloom_hashes,
    V2MatchRecord*  d_matches,
    uint32_t*       d_match_count,
    cudaStream_t    stream)
{
    constexpr int BLOCK = 128;
    int blocks = (int)((count + BLOCK - 1) / BLOCK);
    // Default bloom seed (same as production h160_bloom_filter.cu seed)
    constexpr uint32_t kBloomSeed = 42u;

    if (count > 0) {
        cudaError_t rc = cudaMemsetAsync(d_match_count, 0, sizeof(uint32_t), stream);
        if (rc != cudaSuccess) return rc;
    }

    using AddrLauncher = cudaError_t (*)(
        const uint8_t*, const uint32_t*, const uint32_t*, size_t,
        uint32_t, const uint8_t*, uint64_t, int, uint32_t,
        V2MatchRecord*, uint32_t*, int, int, cudaStream_t);

    auto make_addr_launcher = []<DerivationScheme S>() -> AddrLauncher {
        return [](const uint8_t* pw, const uint32_t* off, const uint32_t* len, size_t cnt,
                  uint32_t amask, const uint8_t* bloom, uint64_t bbits, int bhashes, uint32_t bseed,
                  V2MatchRecord* matches, uint32_t* mc, int blks, int bsz, cudaStream_t s) -> cudaError_t {
            v2_addr_kernel<S><<<blks, bsz, 0, s>>>(pw, off, len, cnt, amask, bloom, bbits, bhashes, bseed, matches, mc);
            return cudaGetLastError();
        };
    };

    struct Entry { DerivationScheme scheme; AddrLauncher launcher; };
    const Entry kEntries[] = {
        {DerivationScheme::SHA256_PW,          make_addr_launcher.template operator()<DerivationScheme::SHA256_PW>()},
        {DerivationScheme::SHA256_SHA256_PW,   make_addr_launcher.template operator()<DerivationScheme::SHA256_SHA256_PW>()},
        {DerivationScheme::SHA256_PW_NEWLINE,  make_addr_launcher.template operator()<DerivationScheme::SHA256_PW_NEWLINE>()},
        {DerivationScheme::SHA256_PW_PW,       make_addr_launcher.template operator()<DerivationScheme::SHA256_PW_PW>()},
        {DerivationScheme::SHA256_SHA256_PW_PW,make_addr_launcher.template operator()<DerivationScheme::SHA256_SHA256_PW_PW>()},
        {DerivationScheme::SHA256_ITER_16,     make_addr_launcher.template operator()<DerivationScheme::SHA256_ITER_16>()},
        {DerivationScheme::HMAC_SHA512_PW,     make_addr_launcher.template operator()<DerivationScheme::HMAC_SHA512_PW>()},
        {DerivationScheme::SHA512_PW_HALF,     make_addr_launcher.template operator()<DerivationScheme::SHA512_PW_HALF>()},
    };

    for (const auto& e : kEntries) {
        if (!(scheme_mask & scheme_bit(e.scheme))) continue;
        cudaError_t rc = e.launcher(
            d_passphrases, d_offsets, d_lengths, count,
            addr_mask, d_bloom, bloom_bits, bloom_hashes, kBloomSeed,
            d_matches, d_match_count, blocks, BLOCK, stream);
        if (rc != cudaSuccess) return rc;
    }
    return cudaSuccess;
}

// v2_init / v2_shutdown / v2_set_puzzle_targets / v2_brain_wallet_batch
// have C++ linkage to match the namespaced declarations in
// brain_wallet_v2.hpp. (An earlier extern "C" wrapper here caused a header /
// definition linkage mismatch -- caught by code review and removed.)

cudaError_t v2_init(cudaStream_t /*stream*/) {
    // No persistent state needed for puzzle-only mode (constant memory is
    // initialized per-call by v2_set_puzzle_targets). Once we add full brain-
    // wallet mode, this is where we'll preallocate the EC table and persistent
    // device buffers.
    return cudaSuccess;
}

void v2_shutdown() {
    // Nothing to free yet.
}

cudaError_t v2_set_puzzle_targets(const std::vector<PuzzleTarget>& targets) {
    if (targets.size() > PUZZLE_TARGET_MAX) return cudaErrorInvalidValue;
    int n = static_cast<int>(targets.size());
    if (n > 0) {
        cudaError_t err = cudaMemcpyToSymbol(
            c_puzzle_targets, targets.data(),
            sizeof(PuzzleTarget) * n, 0, cudaMemcpyHostToDevice);
        if (err != cudaSuccess) return err;
    }
    cudaError_t err = cudaMemcpyToSymbol(c_puzzle_target_count, &n, sizeof(int),
                                         0, cudaMemcpyHostToDevice);
    return err;
}

cudaError_t v2_brain_wallet_batch(
    const uint8_t* d_passphrases,
    const uint32_t* d_offsets,
    const uint32_t* d_lengths,
    size_t count,
    uint32_t scheme_mask,
    uint32_t addr_mask,
    const uint8_t* /*d_bloom*/,
    uint64_t /*bloom_bits*/,
    int /*bloom_hashes*/,
    V2MatchRecord* d_matches,
    uint32_t* d_match_count,
    cudaStream_t stream)
{
    // Phase 3 (v1.4.0): puzzle-only path supports all 8 DerivationScheme values.
    // Phase 4 (v1.5.0): addr_mask != 0 path — derive private key, EC multiply,
    // run multi-address bloom check. Uses a fused per-thread kernel.
    if (addr_mask != 0) {
        return v2_brain_wallet_addr_batch(
            d_passphrases, d_offsets, d_lengths, count,
            scheme_mask, addr_mask,
            d_bloom, bloom_bits, bloom_hashes,
            d_matches, d_match_count, stream);
    }
    if (scheme_mask == 0) {
        return cudaErrorInvalidValue;
    }

    constexpr int BLOCK = 256;
    const int blocks = static_cast<int>((count + BLOCK - 1) / BLOCK);

    // Reset the match counter at the start of every batch. Without this
    // the kernel's atomicAdd into d_match_count keeps growing across
    // batches; the host then reads stale match records, and once the
    // counter exceeds d_matches' capacity the kernel walks off the end.
    // (Gemini PR #15 HIGH finding.)
    if (count > 0) {
        cudaError_t rc = cudaMemsetAsync(d_match_count, 0,
                                         sizeof(uint32_t), stream);
        if (rc != cudaSuccess) return rc;
    }

    // Per-scheme dispatch: one kernel launch per enabled scheme bit.
    // Stops at the first launch error to avoid burying it under later
    // launches' errors.
    //
    // The kernel template is instantiated per DerivationScheme value, so
    // the dispatch table is built statically as a fold over the enum. New
    // schemes plug in by adding one entry to kSchemes; the if-chain that
    // used to live here mostly auto-grew on each phase landing and was
    // cumulative copy-paste.
    using SchemeLauncher = cudaError_t (*)(
        const uint8_t*, const uint32_t*, const uint32_t*, size_t,
        V2MatchRecord*, uint32_t*, int, int, cudaStream_t);

    auto make_launcher = []<DerivationScheme S>() -> SchemeLauncher {
        return [](const uint8_t* d_passphrases,
                  const uint32_t* d_offsets,
                  const uint32_t* d_lengths,
                  size_t count,
                  V2MatchRecord* d_matches,
                  uint32_t* d_match_count,
                  int blocks, int block_size,
                  cudaStream_t stream) -> cudaError_t {
            v2_puzzle_only_kernel_scheme<S>
                <<<blocks, block_size, 0, stream>>>(
                    d_passphrases, d_offsets, d_lengths, count,
                    d_matches, d_match_count);
            return cudaGetLastError();
        };
    };

    struct SchemeEntry {
        DerivationScheme scheme;
        SchemeLauncher   launcher;
    };
    const SchemeEntry kSchemes[] = {
        { DerivationScheme::SHA256_PW,
          make_launcher.template operator()<DerivationScheme::SHA256_PW>() },
        { DerivationScheme::SHA256_SHA256_PW,
          make_launcher.template operator()<DerivationScheme::SHA256_SHA256_PW>() },
        { DerivationScheme::SHA256_PW_NEWLINE,
          make_launcher.template operator()<DerivationScheme::SHA256_PW_NEWLINE>() },
        { DerivationScheme::SHA256_PW_PW,
          make_launcher.template operator()<DerivationScheme::SHA256_PW_PW>() },
        { DerivationScheme::SHA256_SHA256_PW_PW,
          make_launcher.template operator()<DerivationScheme::SHA256_SHA256_PW_PW>() },
        { DerivationScheme::SHA256_ITER_16,
          make_launcher.template operator()<DerivationScheme::SHA256_ITER_16>() },
        // SHA-512-based schemes (Phase 3 finish, v1.4.0): device::sha512
        // and device::hmac_sha512 live in device_hashes.cuh.
        { DerivationScheme::HMAC_SHA512_PW,
          make_launcher.template operator()<DerivationScheme::HMAC_SHA512_PW>() },
        { DerivationScheme::SHA512_PW_HALF,
          make_launcher.template operator()<DerivationScheme::SHA512_PW_HALF>() },
    };

    for (const auto& entry : kSchemes) {
        if ((scheme_mask & scheme_bit(entry.scheme)) == 0) continue;
        const cudaError_t rc = entry.launcher(
            d_passphrases, d_offsets, d_lengths, count,
            d_matches, d_match_count, blocks, BLOCK, stream);
        if (rc != cudaSuccess) return rc;
    }
    return cudaSuccess;
}

// v2_weak_prng_brute lives in src/gpu/v2/weak_prng_kernel.cu (Phase 5).

// ---------------------------------------------------------------------------
// BIP-39 kernel: per-thread PBKDF2(mnemonic, salt, 2048) → BIP-32 root key
// → optional path derivation → EC multiply → puzzle-target or bloom check.
//
// WARNING: PBKDF2 with 2048 HMAC-SHA-512 rounds is computation-heavy per
// thread. Expected throughput ~1-5K mnemonics/sec/GPU at typical batch sizes.
// ---------------------------------------------------------------------------

// Device constant: passphrase suffix for BIP-39 salt ("mnemonic" + passphrase)
// The host copies the full salt into d_bip39_salt via constant memory or
// passes it as a pointer. Max passphrase length is 200 bytes.
__device__ __constant__ uint8_t  c_bip39_salt[212];  // "mnemonic"(8) + passphrase(≤200) + padding
__device__ __constant__ uint32_t c_bip39_salt_len;

// Device constant: BIP-32 derivation path (up to 8 components)
// Each entry: bit 31 = hardened flag, bits 0-30 = index.
// A zero path_len means use the root key directly.
__device__ __constant__ uint32_t c_bip32_path[8];
__device__ __constant__ uint32_t c_bip32_path_len;

// Derive a compressed pubkey from a 32-byte private key scalar.
// Returns pub[33]: 02/03 prefix + 32-byte X.
__device__ static void priv_to_compressed_pub(
    const uint8_t priv[32], uint8_t pub_compressed[33])
{
    uint256 scalar;
    #pragma unroll
    for (int i = 0; i < 8; ++i) {
        uint32_t w = 0;
        int base = (7 - i) * 4;
        w |= (uint32_t)priv[base]     << 24;
        w |= (uint32_t)priv[base + 1] << 16;
        w |= (uint32_t)priv[base + 2] <<  8;
        w |= (uint32_t)priv[base + 3];
        scalar.limbs[i] = w;
    }
    ECPointAffine P;
    ec_mul_simple(P, scalar);
    // Compressed: 02 if Y even, 03 if Y odd
    pub_compressed[0] = (P.y.limbs[0] & 1u) ? 0x03 : 0x02;
    #pragma unroll
    for (int i = 0; i < 8; ++i) {
        uint32_t lv = P.x.limbs[7 - i];
        pub_compressed[1 + i * 4 + 0] = (uint8_t)(lv >> 24);
        pub_compressed[1 + i * 4 + 1] = (uint8_t)(lv >> 16);
        pub_compressed[1 + i * 4 + 2] = (uint8_t)(lv >>  8);
        pub_compressed[1 + i * 4 + 3] = (uint8_t)(lv      );
    }
}

__global__ __launch_bounds__(64, 2) void v2_bip39_kernel(
    const uint8_t*  __restrict__ d_mnemonics,
    const uint32_t* __restrict__ d_offsets,
    const uint32_t* __restrict__ d_lengths,
    size_t count,
    V2MatchRecord*  __restrict__ d_matches,
    uint32_t*       __restrict__ d_match_count)
{
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    const uint8_t* mnemonic = d_mnemonics + d_offsets[idx];
    uint32_t       mn_len   = d_lengths[idx];

    // Step 1: PBKDF2-HMAC-SHA512(mnemonic, salt, 2048) → 64-byte BIP-39 seed
    uint8_t seed[64];
    device::pbkdf2_hmac_sha512_one_block(
        mnemonic, mn_len,
        c_bip39_salt, c_bip39_salt_len,
        2048u,
        seed);

    // Step 2: BIP-32 root key
    uint8_t master_key[32], master_chain[32];
    device::bip32_root_key(seed, master_key, master_chain);

    // Step 3: Follow derivation path
    uint8_t cur_key[32], cur_chain[32];
    for (int i = 0; i < 32; ++i) { cur_key[i] = master_key[i]; cur_chain[i] = master_chain[i]; }

    uint32_t path_len = c_bip32_path_len;
    uint8_t  parent_pub[33];

    for (uint32_t d = 0; d < path_len && d < 8u; ++d) {
        uint32_t component = c_bip32_path[d];
        uint8_t  next_key[32], next_chain[32];
        if (component & 0x80000000u) {
            // Hardened
            device::bip32_derive_child_hardened(cur_key, cur_chain, component, next_key, next_chain);
        } else {
            // Non-hardened: need parent compressed pubkey
            priv_to_compressed_pub(cur_key, parent_pub);
            device::bip32_derive_child_nonhardened(cur_key, cur_chain, parent_pub, component, next_key, next_chain);
        }
        for (int i = 0; i < 32; ++i) { cur_key[i] = next_key[i]; cur_chain[i] = next_chain[i]; }
    }

    // cur_key is the final private key. Check against puzzle targets.
    uint64_t hash_limbs[4];
    v2_hash_to_limbs(cur_key, hash_limbs);

    const int n_targets = c_puzzle_target_count;
    for (int ti = 0; ti < n_targets; ++ti) {
        const PuzzleTarget& t = c_puzzle_targets[ti];
        bool match = true;
        #pragma unroll
        for (int j = 0; j < 4; ++j) {
            if ((hash_limbs[j] & t.low_mask[j]) != t.low_value[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            uint32_t slot = atomicAdd(d_match_count, 1u);
            if (slot < V2_MAX_MATCHES_PER_BATCH) {
                V2MatchRecord rec{};
                rec.pp_idx    = (uint32_t)idx;
                rec.puzzle_n  = t.puzzle_n;
                rec.scheme_id = (uint8_t)DerivationScheme::SHA256_PW;  // BIP-39 sentinel
                rec.kind      = (uint8_t)V2MatchRecord::Kind::PUZZLE_KEY_HIT;
                d_matches[slot] = rec;
            }
        }
    }
}

cudaError_t v2_bip39_brute(
    const uint8_t*  d_mnemonics,
    const uint32_t* d_offsets,
    const uint32_t* d_lengths,
    size_t          count,
    const std::string& bip39_passphrase,
    const std::vector<std::vector<uint32_t>>& parsed_paths,
    uint32_t        addr_mask,
    const uint8_t*  /*d_bloom*/,
    uint64_t        /*bloom_bits*/,
    int             /*bloom_hashes*/,
    V2MatchRecord*  d_matches,
    uint32_t*       d_match_count,
    cudaStream_t    stream)
{
    if (count == 0) return cudaSuccess;

    // Build the BIP-39 salt: "mnemonic" + passphrase
    uint8_t salt_buf[212] = {};
    const char kPrefix[] = "mnemonic";
    for (int i = 0; i < 8; ++i) salt_buf[i] = (uint8_t)kPrefix[i];
    uint32_t salt_len = 8u;
    if (!bip39_passphrase.empty()) {
        uint32_t pp_len = (uint32_t)bip39_passphrase.size();
        if (pp_len > 200u) pp_len = 200u;
        for (uint32_t i = 0; i < pp_len; ++i) salt_buf[8 + i] = (uint8_t)bip39_passphrase[i];
        salt_len += pp_len;
    }
    cudaError_t err;
    err = cudaMemcpyToSymbolAsync(c_bip39_salt, salt_buf, sizeof(salt_buf), 0,
                                  cudaMemcpyHostToDevice, stream);
    if (err != cudaSuccess) return err;
    err = cudaMemcpyToSymbolAsync(c_bip39_salt_len, &salt_len, sizeof(uint32_t), 0,
                                  cudaMemcpyHostToDevice, stream);
    if (err != cudaSuccess) return err;

    // Derive path: use first path from parsed_paths, or empty (root key)
    uint32_t path_buf[8] = {};
    uint32_t path_len = 0u;
    if (!parsed_paths.empty() && !parsed_paths[0].empty()) {
        path_len = (uint32_t)std::min(parsed_paths[0].size(), (size_t)8u);
        for (uint32_t i = 0; i < path_len; ++i) path_buf[i] = parsed_paths[0][i];
    }
    err = cudaMemcpyToSymbolAsync(c_bip32_path, path_buf, sizeof(path_buf), 0,
                                  cudaMemcpyHostToDevice, stream);
    if (err != cudaSuccess) return err;
    err = cudaMemcpyToSymbolAsync(c_bip32_path_len, &path_len, sizeof(uint32_t), 0,
                                  cudaMemcpyHostToDevice, stream);
    if (err != cudaSuccess) return err;

    err = cudaMemsetAsync(d_match_count, 0, sizeof(uint32_t), stream);
    if (err != cudaSuccess) return err;

    // addr_mask ignored for now (Phase 4 only checks puzzle targets)
    constexpr int BLOCK = 64;
    int blocks = (int)((count + BLOCK - 1) / BLOCK);
    v2_bip39_kernel<<<blocks, BLOCK, 0, stream>>>(
        d_mnemonics, d_offsets, d_lengths, count, d_matches, d_match_count);
    return cudaGetLastError();
}

}  // namespace v2
}  // namespace gpu
}  // namespace starminer
