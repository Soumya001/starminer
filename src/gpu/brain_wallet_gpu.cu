/**
 * brain_wallet_gpu.cu — MultiGPUBrainWallet implementation (CUDA).
 *
 * Wraps the v2 brain-wallet kernel pipeline (v2_brain_wallet_batch) behind
 * the clean C++ interface declared in brain_wallet_gpu.hpp. All CUDA runtime
 * types are confined to this TU so host .cpp files see only plain C++ types.
 */

#include "brain_wallet_gpu.hpp"
#include "v2/brain_wallet_v2.hpp"

#include <cuda_runtime.h>

#include <cstring>
#include <vector>
#include <string>

namespace starminer {
namespace gpu {

// ---------------------------------------------------------------------------
// Pimpl — all CUDA allocations live here.
// ---------------------------------------------------------------------------

struct MultiGPUBrainWallet::Impl {
    // Device bloom filter (nullable — benchmarks use nullptr / all-zero)
    uint8_t*  d_bloom       = nullptr;
    uint64_t  bloom_bits    = 0;
    int       bloom_hashes  = 0;

    // Output buffers (allocated in init)
    v2::V2MatchRecord* d_matches     = nullptr;
    uint32_t*          d_match_count = nullptr;

    bool initialized = false;

    Impl()  = default;
    ~Impl() {
        cudaFree(d_bloom);
        cudaFree(d_matches);
        cudaFree(d_match_count);
    }
};

// ---------------------------------------------------------------------------

MultiGPUBrainWallet::MultiGPUBrainWallet(const Config& cfg)
    : cfg_(cfg), impl_(nullptr) {}

MultiGPUBrainWallet::~MultiGPUBrainWallet() = default;

bool MultiGPUBrainWallet::init()
{
    impl_ = std::make_unique<Impl>();

    if (v2::v2_init(nullptr) != cudaSuccess) return false;

    if (cudaMalloc(&impl_->d_matches,
                   v2::V2_MAX_MATCHES_PER_BATCH * sizeof(v2::V2MatchRecord))
        != cudaSuccess) return false;

    if (cudaMalloc(&impl_->d_match_count, sizeof(uint32_t)) != cudaSuccess)
        return false;

    impl_->initialized = true;
    return true;
}

bool MultiGPUBrainWallet::load_bloom_filter(
    const uint8_t* data, size_t size,
    uint64_t bits, int k, uint32_t /*seed*/)
{
    if (!impl_ || !impl_->initialized) return false;

    cudaFree(impl_->d_bloom);
    impl_->d_bloom = nullptr;

    if (size > 0) {
        if (cudaMalloc(&impl_->d_bloom, size) != cudaSuccess) return false;
        if (cudaMemcpy(impl_->d_bloom, data, size, cudaMemcpyHostToDevice)
            != cudaSuccess) return false;
    }

    impl_->bloom_bits   = bits;
    impl_->bloom_hashes = k;
    return true;
}

MultiGPUBrainWallet::BatchResult
MultiGPUBrainWallet::process_batch(const std::vector<std::string>& batch)
{
    BatchResult result;
    result.processed = batch.size();

    if (!impl_ || !impl_->initialized || batch.empty()) return result;

    // Pack passphrases into a contiguous byte array
    std::vector<uint8_t>  packed;
    std::vector<uint32_t> offsets(batch.size());
    std::vector<uint32_t> lengths(batch.size());

    uint32_t off = 0;
    for (size_t i = 0; i < batch.size(); ++i) {
        offsets[i] = off;
        lengths[i] = static_cast<uint32_t>(batch[i].size());
        packed.insert(packed.end(), batch[i].begin(), batch[i].end());
        off += lengths[i];
    }
    if (packed.empty()) return result;

    uint8_t*  d_pw  = nullptr;
    uint32_t* d_off = nullptr;
    uint32_t* d_len = nullptr;
    bool ok = true;

    ok &= (cudaMalloc(&d_pw,  packed.size())                        == cudaSuccess);
    ok &= (cudaMalloc(&d_off, batch.size() * sizeof(uint32_t))      == cudaSuccess);
    ok &= (cudaMalloc(&d_len, batch.size() * sizeof(uint32_t))      == cudaSuccess);

    if (ok) {
        cudaMemcpy(d_pw,  packed.data(),  packed.size(),
                   cudaMemcpyHostToDevice);
        cudaMemcpy(d_off, offsets.data(), batch.size() * sizeof(uint32_t),
                   cudaMemcpyHostToDevice);
        cudaMemcpy(d_len, lengths.data(), batch.size() * sizeof(uint32_t),
                   cudaMemcpyHostToDevice);

        const uint32_t zero = 0;
        cudaMemcpy(impl_->d_match_count, &zero, sizeof(zero),
                   cudaMemcpyHostToDevice);

        const uint32_t addr_mask = impl_->d_bloom ? v2::ADDR_MASK_ALL : 0u;

        v2::v2_brain_wallet_batch(
            d_pw, d_off, d_len, batch.size(),
            v2::SCHEME_MASK_ALL,
            addr_mask,
            impl_->d_bloom,
            impl_->bloom_bits,
            impl_->bloom_hashes,
            impl_->d_matches,
            impl_->d_match_count,
            nullptr   // default (null) stream — synchronous
        );
        cudaDeviceSynchronize();
    }

    cudaFree(d_pw);
    cudaFree(d_off);
    cudaFree(d_len);
    return result;
}

}  // namespace gpu
}  // namespace starminer
