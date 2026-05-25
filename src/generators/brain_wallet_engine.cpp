/**
 * brain_wallet_engine.cpp — stub for CPU/non-GPU builds.
 *
 * The real engine lives in the GPU v2 kernel pipeline.
 * This file satisfies the linker when the GPU v2 kernels are absent.
 */
#ifdef STARMINER_PRO
// Empty stub — all symbols provided by starminer_gpu on CUDA/ROCm/Metal builds.
#endif
