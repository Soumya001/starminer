/**
 * brain_wallet_runner.cpp — stub implementation.
 *
 * The full brain wallet mode requires the GPU v2 orchestrator and bloom
 * filter infrastructure. This stub returns a clear error on CPU-only builds.
 */
#ifdef STARMINER_PRO

#include "runtime/brain_wallet_runner.hpp"
#include "cli/cli_parser.hpp"
#include <iostream>

namespace starminer {
namespace runtime {

int run_brain_wallet_mode(const Arguments& args) {
#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM) || defined(STARMINER_USE_METAL)
    std::cerr << "[BrainWallet] GPU orchestrator not linked in this build.\n";
    std::cerr << "              Rebuild with -DSTARMINER_USE_CUDA=ON (or ROCm/Metal).\n";
    return 1;
#else
    std::cerr << "[BrainWallet] Brain wallet mode requires a GPU backend.\n";
    std::cerr << "              This is a CPU-only build. Rebuild with GPU support.\n";
    return 1;
#endif
}

}  // namespace runtime
}  // namespace starminer

#endif  // STARMINER_PRO
