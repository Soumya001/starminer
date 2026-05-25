/**
 * streaming_brain_wallet.cpp — stub for CPU/non-GPU builds.
 *
 * The real streaming brain wallet generator requires the v2 pipeline.
 * This stub satisfies the linker when GPU kernels are absent.
 */
#ifdef STARMINER_PRO
// Empty stub — all symbols provided by starminer_gpu on CUDA/ROCm/Metal builds.
#endif
