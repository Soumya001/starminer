#pragma once
/**
 * MetalBrainWalletSolver — Apple Metal brain wallet bloom-filter scanner.
 * Compiled only on macOS (STARMINER_USE_METAL).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace starminer {
namespace gpu {

struct MetalBrainWalletHit {
    std::string passphrase;
    uint8_t     scheme_id = 0;
};

class MetalBrainWalletSolver {
public:
    MetalBrainWalletSolver();
    ~MetalBrainWalletSolver();

    MetalBrainWalletSolver(const MetalBrainWalletSolver&) = delete;
    MetalBrainWalletSolver& operator=(const MetalBrainWalletSolver&) = delete;

    bool init();
    bool load_bloom_filter(const uint8_t* data, size_t size,
                           uint64_t bits, uint32_t num_hashes, uint32_t seed);

    bool process_batch(const std::vector<std::string>& batch,
                       std::vector<MetalBrainWalletHit>& hits_out);

    const std::string& error() const { return error_; }

private:
    struct Impl;
    Impl*       impl_  = nullptr;
    std::string error_;
};

}  // namespace gpu
}  // namespace starminer
