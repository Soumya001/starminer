#pragma once
// Stub: GPU brainwallet pipeline interface.
// Full CUDA implementation is not required for pool mining mode.

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

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

    struct BatchResult {
        uint64_t processed = 0;
    };

    explicit MultiGPUBrainWallet(const Config& /*cfg*/) {}

    bool init() { return false; }

    bool load_bloom_filter(const uint8_t* /*data*/, size_t /*size*/,
                           uint64_t /*bits*/, int /*k*/, uint32_t /*seed*/) {
        return false;
    }

    BatchResult process_batch(const std::vector<std::string>& batch) {
        BatchResult r;
        r.processed = batch.size();
        return r;
    }
};

}  // namespace gpu
}  // namespace starminer
