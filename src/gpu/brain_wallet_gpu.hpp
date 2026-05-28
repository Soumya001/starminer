#pragma once
/**
 * MultiGPUBrainWallet — host-facing API for the GPU v2 brain-wallet pipeline.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace starminer {
namespace gpu {

class MultiGPUBrainWallet {
public:
    struct Config {
        std::vector<int> gpu_ids;
        int  batch_size            = 65536;
        int  max_passphrase_length = 256;
        bool store_private_keys    = false;
    };

    struct Hit {
        std::string passphrase;
        uint8_t     scheme_id = 0;
    };

    struct BatchResult {
        uint64_t        processed = 0;
        std::vector<Hit> hits;
    };

    explicit MultiGPUBrainWallet(const Config& cfg);
    ~MultiGPUBrainWallet();

    // Non-copyable, movable
    MultiGPUBrainWallet(const MultiGPUBrainWallet&)            = delete;
    MultiGPUBrainWallet& operator=(const MultiGPUBrainWallet&) = delete;
    MultiGPUBrainWallet(MultiGPUBrainWallet&&)                 = default;
    MultiGPUBrainWallet& operator=(MultiGPUBrainWallet&&)      = default;

    /**
     * Initialise the v2 GPU pipeline (EC table precompute, device allocations).
     * Returns false if no compatible GPU is found or allocation fails.
     */
    bool init();

    /**
     * Upload a bloom filter to device memory. Must be called after init().
     * `seed` must match the seed used when building the .blf file.
     */
    bool load_bloom_filter(const uint8_t* data, size_t size,
                           uint64_t bits, int k, uint32_t seed);

    /**
     * Run a batch of passphrases through the full pipeline
     * (SHA-256 → secp256k1 → RIPEMD-160 → bloom probe).
     * Blocks until the GPU finishes.
     */
    BatchResult process_batch(const std::vector<std::string>& batch);

private:
    Config cfg_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
}  // namespace starminer
