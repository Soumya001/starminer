/**
 * brain_wallet_metal.mm — MetalBrainWalletSolver implementation.
 *
 * Loads puzzle.metal (which has secp256k1 + SHA-256 + RIPEMD-160) and
 * appends a brain_wallet_bloom_search kernel that:
 *   1. SHA-256 each passphrase → private key
 *   2. k*G via precomputed 8-bit window G-table
 *   3. Compress pubkey → SHA-256 → RIPEMD-160 → h160
 *   4. Kirsch-Mitzenmacher MurmurHash3 bloom probe
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "brain_wallet_metal.hpp"
#include "puzzle_metal.hpp"          // kPuzzleMetalGTableUlongs / kPuzzleMetalGTableBytes
#include "../core/types.hpp"
#include "v2/brain_wallet_v2.hpp"    // V2MatchRecord, V2_MAX_MATCHES_PER_BATCH

// cpu:: secp256k1 host implementation for G-table build
#include "../core/crypto_cpu.hpp"

namespace starminer {
namespace gpu {

// ---------------------------------------------------------------------------
// The brain-wallet bloom kernel appended to puzzle.metal's source at runtime.
// puzzle.metal already provides: sha256_long, scalar_mul_g, mod_inv, mod_sqr,
// mod_mul, compress_pubkey, sha256_33bytes, ripemd160_32bytes, limbs_zero.
// ---------------------------------------------------------------------------
static const char* kBloomKernelSource = R"MSL(
struct V2MatchRecord_BW {
    uint   pp_idx;
    ulong  weak_seed;
    ushort puzzle_n;
    uchar  scheme_id;
    uchar  addr_type;
    uchar  kind;
    uchar  reserved[3];
};
constant uint kBWMaxMatches = 4096u;

// MurmurHash3 32-bit for exactly 20-byte input (h160).
// Matches h160_bloom_filter.cu :: murmur3_20.
inline uint murmur3_20_bw(thread const uchar data[20], uint seed) {
    const uint c1 = 0xcc9e2d51u;
    const uint c2 = 0x1b873593u;
    uint h1 = seed;
    for (int i = 0; i < 5; ++i) {
        uint k1 = (uint)data[i*4+0]
                | ((uint)data[i*4+1] << 8)
                | ((uint)data[i*4+2] << 16)
                | ((uint)data[i*4+3] << 24);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5u + 0xe6546b64u;
    }
    h1 ^= 20u;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13; h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;
    return h1;
}

inline bool bloom_check_bw(
    thread const uchar h160[20],
    device const uchar* bloom_data,
    ulong bloom_bits,
    uint  num_hashes,
    uint  seed)
{
    ulong run  = (ulong)murmur3_20_bw(h160, seed);
    ulong step = (ulong)murmur3_20_bw(h160, seed ^ 0x5f3759dfu);
    for (uint i = 0; i < num_hashes; ++i) {
        ulong bit_idx  = run % bloom_bits;
        ulong byte_idx = bit_idx >> 3;
        uint  bit_mask = 1u << (uint)(bit_idx & 7u);
        if ((bloom_data[byte_idx] & bit_mask) == 0) return false;
        run += step;
    }
    return true;
}

kernel void brain_wallet_bloom_search(
    device const ulong*            g_table     [[buffer(0)]],
    device const uchar*            passphrases [[buffer(1)]],
    device const uint*             offsets     [[buffer(2)]],
    device const uint*             lengths     [[buffer(3)]],
    device const uchar*            bloom_data  [[buffer(4)]],
    constant     ulong&            bloom_bits  [[buffer(5)]],
    constant     uint&             num_hashes  [[buffer(6)]],
    constant     uint&             bloom_seed  [[buffer(7)]],
    device       atomic_uint*      match_count [[buffer(8)]],
    device       V2MatchRecord_BW* matches     [[buffer(9)]],
    constant     uint&             pw_count    [[buffer(10)]],
    uint                           tid         [[thread_position_in_grid]])
{
    if (tid >= pw_count) return;

    uint len = lengths[tid];
    if (len == 0 || len > 256) return;

    // Copy passphrase from device to thread-local (sha256_long needs thread ptr)
    uchar pw_local[256];
    device const uchar* pw_src = passphrases + offsets[tid];
    for (uint i = 0; i < len; ++i) pw_local[i] = pw_src[i];

    // Passphrase → SHA-256 → private key
    uint sha_state[8];
    sha256_long(pw_local, len, sha_state);

    // Pack big-endian SHA-256 words into LE-by-limb key (matches puzzle.metal layout)
    ulong k[4];
    k[0] = ((ulong)sha_state[7] << 32) | (ulong)sha_state[6];
    k[1] = ((ulong)sha_state[5] << 32) | (ulong)sha_state[4];
    k[2] = ((ulong)sha_state[3] << 32) | (ulong)sha_state[2];
    k[3] = ((ulong)sha_state[1] << 32) | (ulong)sha_state[0];

    if (limbs_zero(k)) return;

    // k * G (windowed 8-bit table)
    ulong px[4], py[4], pz[4];
    scalar_mul_g(px, py, pz, k, g_table);
    if (limbs_zero(pz)) return;

    // Affinize
    ulong z_inv[4], z_inv2[4], z_inv3[4];
    mod_inv(z_inv, pz);
    mod_sqr(z_inv2, z_inv);
    mod_mul(z_inv3, z_inv2, z_inv);
    ulong ax[4], ay[4];
    mod_mul(ax, px, z_inv2);
    mod_mul(ay, py, z_inv3);

    // Compressed pubkey → SHA-256 → RIPEMD-160
    uchar pubkey[33], sha[32], h160[20];
    compress_pubkey(pubkey, ax, ay);
    sha256_33bytes(pubkey, sha);
    ripemd160_32bytes(sha, h160);

    if (!bloom_check_bw(h160, bloom_data, bloom_bits, num_hashes, bloom_seed)) return;

    uint slot = atomic_fetch_add_explicit(match_count, 1u, memory_order_relaxed);
    if (slot < kBWMaxMatches) {
        matches[slot].pp_idx    = tid;
        matches[slot].scheme_id = 0;
        matches[slot].kind      = 0;
        matches[slot].puzzle_n  = 0;
    }
}
)MSL";

// ---------------------------------------------------------------------------
// G-table build (same logic as puzzle_metal.mm; duplicated here to keep
// this TU self-contained without exposing the static helper there).
// ---------------------------------------------------------------------------
static void bw_build_g_table(std::vector<uint64_t>& out)
{
    out.assign(kPuzzleMetalGTableUlongs, 0ull);
    for (uint32_t w = 0; w < kPuzzleMetalGTableWindows; ++w) {
        cpu::uint256_t base_scalar{};
        const uint32_t bit  = 8u * w;
        const uint32_t limb = bit / 64u;
        const uint32_t off  = bit % 64u;
        base_scalar.d[limb] = (uint64_t)1 << off;

        cpu::ECPoint base_point;
        cpu::ec_mul(base_point, base_scalar);
        cpu::uint256_t base_x, base_y;
        cpu::ec_to_affine(base_x, base_y, base_point);

        cpu::ECPoint acc;
        acc.X = base_x;
        acc.Y = base_y;
        acc.Z = cpu::uint256_t(1);

        for (uint32_t d = 1; d < kPuzzleMetalGTableEntries; ++d) {
            cpu::uint256_t ax, ay;
            cpu::ec_to_affine(ax, ay, acc);
            const size_t base_idx =
                (size_t)(w * kPuzzleMetalGTableEntries + d) * 8u;
            for (int i = 0; i < 4; ++i) out[base_idx + i]     = ax.d[i];
            for (int i = 0; i < 4; ++i) out[base_idx + 4 + i] = ay.d[i];
            if (d + 1 < kPuzzleMetalGTableEntries) {
                cpu::ECPoint next;
                cpu::ec_add(next, acc, base_x, base_y);
                acc = next;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Load puzzle.metal from disk (same search paths as v2_metal_dispatch.mm)
// ---------------------------------------------------------------------------
static NSString* load_puzzle_metal_source()
{
    NSBundle* main = [NSBundle mainBundle];
    NSString* candidates[] = {
        [main pathForResource:@"puzzle" ofType:@"metal"],
        [[main bundlePath] stringByAppendingPathComponent:@"puzzle.metal"],
        @"./puzzle.metal",
        @"./src/gpu/puzzle.metal",
    };
    for (NSString* p : candidates) {
        if (!p) continue;
        NSError* err = nil;
        NSString* src = [NSString stringWithContentsOfFile:p
                                                  encoding:NSUTF8StringEncoding
                                                     error:&err];
        if (src) return src;
    }
    return nil;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct MetalBrainWalletSolver::Impl {
    id<MTLDevice>               device      = nil;
    id<MTLCommandQueue>         queue       = nil;
    id<MTLComputePipelineState> pipe        = nil;

    id<MTLBuffer> b_gtable      = nil;
    id<MTLBuffer> b_bloom       = nil;
    id<MTLBuffer> b_matches     = nil;
    id<MTLBuffer> b_match_count = nil;

    uint64_t bloom_bits   = 0;
    uint32_t num_hashes   = 0;
    uint32_t bloom_seed   = 42u;

    bool initialized  = false;
    bool bloom_loaded = false;
};

// ---------------------------------------------------------------------------
MetalBrainWalletSolver::MetalBrainWalletSolver() = default;
MetalBrainWalletSolver::~MetalBrainWalletSolver() { delete impl_; }

bool MetalBrainWalletSolver::init()
{
    impl_ = new Impl();

    @autoreleasepool {
        impl_->device = MTLCreateSystemDefaultDevice();
        if (!impl_->device) { error_ = "no Metal device"; return false; }

        // Load puzzle.metal + append bloom kernel
        NSString* base_src = load_puzzle_metal_source();
        if (!base_src) {
            error_ = "could not locate puzzle.metal";
            return false;
        }
        NSString* full_src = [base_src stringByAppendingString:
                              [NSString stringWithUTF8String:kBloomKernelSource]];

        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_4;
        opts.fastMathEnabled = YES;
        NSError* err = nil;
        id<MTLLibrary> lib = [impl_->device newLibraryWithSource:full_src
                                                         options:opts
                                                           error:&err];
        if (!lib) {
            error_ = std::string("MSL compile: ")
                   + (err ? [[err localizedDescription] UTF8String] : "?");
            return false;
        }

        id<MTLFunction> fn = [lib newFunctionWithName:@"brain_wallet_bloom_search"];
        if (!fn) { error_ = "kernel brain_wallet_bloom_search not found"; return false; }

        impl_->pipe = [impl_->device newComputePipelineStateWithFunction:fn error:&err];
        if (!impl_->pipe) {
            error_ = std::string("pipeline: ")
                   + (err ? [[err localizedDescription] UTF8String] : "?");
            return false;
        }

        impl_->queue = [impl_->device newCommandQueue];

        // Build and upload G-table
        std::vector<uint64_t> gtable_host;
        bw_build_g_table(gtable_host);
        impl_->b_gtable = [impl_->device
            newBufferWithBytes:gtable_host.data()
                        length:kPuzzleMetalGTableBytes
                       options:MTLResourceStorageModeShared];
        if (!impl_->b_gtable) { error_ = "G-table buffer alloc failed"; return false; }

        // Allocate persistent output buffers
        impl_->b_matches =
            [impl_->device newBufferWithLength:v2::V2_MAX_MATCHES_PER_BATCH * 24
                                       options:MTLResourceStorageModeShared];
        impl_->b_match_count =
            [impl_->device newBufferWithLength:sizeof(uint32_t)
                                       options:MTLResourceStorageModeShared];
    }

    impl_->initialized = true;
    return true;
}

bool MetalBrainWalletSolver::load_bloom_filter(
    const uint8_t* data, size_t size,
    uint64_t bits, uint32_t num_hashes, uint32_t seed)
{
    if (!impl_ || !impl_->initialized) return false;
    @autoreleasepool {
        impl_->b_bloom = [impl_->device
            newBufferWithBytes:data
                        length:size
                       options:MTLResourceStorageModeShared];
    }
    if (!impl_->b_bloom) return false;
    impl_->bloom_bits  = bits;
    impl_->num_hashes  = num_hashes;
    impl_->bloom_seed  = seed;
    impl_->bloom_loaded = true;
    return true;
}

bool MetalBrainWalletSolver::process_batch(
    const std::vector<std::string>& batch,
    std::vector<MetalBrainWalletHit>& hits_out)
{
    hits_out.clear();
    if (!impl_ || !impl_->initialized || !impl_->bloom_loaded || batch.empty())
        return false;

    @autoreleasepool {
        // Pack passphrases
        std::vector<uint8_t>  packed;
        std::vector<uint32_t> offsets(batch.size()), lengths(batch.size());
        uint32_t off = 0;
        for (size_t i = 0; i < batch.size(); ++i) {
            offsets[i] = off;
            lengths[i] = (uint32_t)batch[i].size();
            packed.insert(packed.end(), batch[i].begin(), batch[i].end());
            off += lengths[i];
        }

        uint32_t pw_count = (uint32_t)batch.size();

        id<MTLBuffer> b_pw  = [impl_->device
            newBufferWithBytes:packed.data()
                        length:packed.size()
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_off = [impl_->device
            newBufferWithBytes:offsets.data()
                        length:offsets.size() * sizeof(uint32_t)
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_len = [impl_->device
            newBufferWithBytes:lengths.data()
                        length:lengths.size() * sizeof(uint32_t)
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_bbits = [impl_->device
            newBufferWithBytes:&impl_->bloom_bits
                        length:sizeof(uint64_t)
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_nhash = [impl_->device
            newBufferWithBytes:&impl_->num_hashes
                        length:sizeof(uint32_t)
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_seed = [impl_->device
            newBufferWithBytes:&impl_->bloom_seed
                        length:sizeof(uint32_t)
                       options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_pwc = [impl_->device
            newBufferWithBytes:&pw_count
                        length:sizeof(uint32_t)
                       options:MTLResourceStorageModeShared];

        // Reset match counter
        *(uint32_t*)[impl_->b_match_count contents] = 0;

        id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:impl_->pipe];
        [enc setBuffer:impl_->b_gtable     offset:0 atIndex:0];
        [enc setBuffer:b_pw                offset:0 atIndex:1];
        [enc setBuffer:b_off               offset:0 atIndex:2];
        [enc setBuffer:b_len               offset:0 atIndex:3];
        [enc setBuffer:impl_->b_bloom      offset:0 atIndex:4];
        [enc setBuffer:b_bbits             offset:0 atIndex:5];
        [enc setBuffer:b_nhash             offset:0 atIndex:6];
        [enc setBuffer:b_seed              offset:0 atIndex:7];
        [enc setBuffer:impl_->b_match_count offset:0 atIndex:8];
        [enc setBuffer:impl_->b_matches    offset:0 atIndex:9];
        [enc setBuffer:b_pwc               offset:0 atIndex:10];

        MTLSize tg   = MTLSizeMake(64, 1, 1);
        MTLSize grid = MTLSizeMake(pw_count, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        uint32_t hits = *(uint32_t*)[impl_->b_match_count contents];
        if (hits > (uint32_t)v2::V2_MAX_MATCHES_PER_BATCH)
            hits = (uint32_t)v2::V2_MAX_MATCHES_PER_BATCH;

        // Match records are 24-byte structs; read pp_idx from offset 0
        const uint8_t* mrecs = (const uint8_t*)[impl_->b_matches contents];
        for (uint32_t i = 0; i < hits; ++i) {
            uint32_t pp_idx;
            memcpy(&pp_idx, mrecs + i * 24, sizeof(uint32_t));
            if (pp_idx < batch.size()) {
                MetalBrainWalletHit h;
                h.passphrase = batch[pp_idx];
                h.scheme_id  = mrecs[i * 24 + 12]; // scheme_id byte offset in V2MatchRecord_BW
                hits_out.push_back(std::move(h));
            }
        }
    }
    return true;
}

}  // namespace gpu
}  // namespace starminer
