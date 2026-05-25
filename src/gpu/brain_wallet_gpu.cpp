/**
 * brain_wallet_gpu.cpp — CPU-stub for MultiGPUBrainWallet.
 *
 * On CUDA/ROCm/Metal builds, the real implementation is provided by the
 * GPU v2 kernel pipeline (starminer_gpu). This file is compiled only on
 * CPU-only or non-GPU builds to satisfy the linker.
 */
#ifdef STARMINER_PRO
#include "gpu/brain_wallet_gpu.hpp"
// MultiGPUBrainWallet is fully inline in brain_wallet_gpu.hpp (stub body).
// Nothing to define here.
#endif
