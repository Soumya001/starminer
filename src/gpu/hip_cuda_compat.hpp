/**
 * HIP/CUDA Portability Layer for StarMiner
 *
 * Include this header instead of <cuda_runtime.h> in all .cu GPU source files.
 * When compiled with hipcc (AMD ROCm), it maps cuda* names to hip* equivalents
 * so the same source compiles on both NVIDIA CUDA and AMD ROCm without changes.
 *
 * Usage pattern in .cu files:
 *   // Before: #include <cuda_runtime.h>
 *   // After:
 *   #include "hip_cuda_compat.hpp"
 */

#pragma once
#include <cstdint>

// ============================================================================
// Platform detection
// ============================================================================

#if defined(__HIPCC__)
// ── AMD ROCm / HIP compilation path ─────────────────────────────────────────
#include <hip/hip_runtime.h>

// Type aliases: map cudaXxx → hipXxx
#define cudaError_t              hipError_t
#define cudaStream_t             hipStream_t
#define cudaEvent_t              hipEvent_t
#define cudaDeviceProp           hipDeviceProp_t
#define cudaMemcpyKind           hipMemcpyKind

// Status / result codes
#define cudaSuccess              hipSuccess
#define cudaErrorMemoryAllocation hipErrorMemoryAllocation

// cudaMemcpy direction constants
#define cudaMemcpyHostToDevice   hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost   hipMemcpyDeviceToHost
#define cudaMemcpyDeviceToDevice hipMemcpyDeviceToDevice
#define cudaMemcpyDefault        hipMemcpyDefault

// cudaHostAlloc flags
#define cudaHostAllocDefault     hipHostMallocDefault
#define cudaHostAllocMapped      hipHostMallocMapped
#define cudaHostAllocPortable    hipHostMallocPortable

// Memory management
#define cudaMalloc               hipMalloc
#define cudaFree                 hipFree
#define cudaMemcpy               hipMemcpy
#define cudaMemcpyAsync          hipMemcpyAsync
#define cudaMemset               hipMemset
#define cudaMemsetAsync          hipMemsetAsync
#define cudaHostAlloc            hipHostMalloc
#define cudaFreeHost             hipHostFree

// Device management
#define cudaSetDevice            hipSetDevice
#define cudaGetDevice            hipGetDevice
#define cudaGetDeviceCount       hipGetDeviceCount
#define cudaGetDeviceProperties  hipGetDeviceProperties
#define cudaDeviceSynchronize    hipDeviceSynchronize
#define cudaGetErrorString       hipGetErrorString

// Stream management
#define cudaStreamCreate         hipStreamCreate
#define cudaStreamDestroy        hipStreamDestroy
#define cudaStreamSynchronize    hipStreamSynchronize
#define cudaStreamDefault        hipStreamDefault
#define cudaStreamNonBlocking    hipStreamNonBlocking

// Event management
#define cudaEventCreate          hipEventCreate
#define cudaEventCreateWithFlags hipEventCreateWithFlags
#define cudaEventDestroy         hipEventDestroy
#define cudaEventRecord          hipEventRecord
#define cudaEventSynchronize     hipEventSynchronize
#define cudaEventElapsedTime     hipEventElapsedTime
#define cudaEventDisableTiming   hipEventDisableTiming

// Misc
#define cudaDeviceReset          hipDeviceReset
#define cudaGetLastError         hipGetLastError
#define cudaPeekAtLastError      hipPeekAtLastError
#define cudaSetDeviceFlags       hipSetDeviceFlags
#define cudaDriverGetVersion     hipDriverGetVersion
#define cudaRuntimeGetVersion    hipRuntimeGetVersion

// Memory
#define cudaFreeAsync            hipFreeAsync
#define cudaMemcpyToSymbol       hipMemcpyToSymbol
#define cudaMemcpyToSymbolAsync  hipMemcpyToSymbolAsync

// Error codes
#define cudaErrorInvalidDevice   hipErrorInvalidDevice
#define cudaErrorInvalidValue    hipErrorInvalidValue
#define cudaErrorNotReady        hipErrorNotReady
#define cudaErrorLaunchFailure   hipErrorLaunchFailure
#define cudaErrorNotSupported    hipErrorNotSupported

// Device flags
#define cudaDeviceScheduleBlockingSync hipDeviceScheduleBlockingSync

// CUDA L2 cache access policy hints — not supported on HIP/AMD, stub as no-ops.
// These only affect performance (L2 cache residency), not correctness.
#define cudaStreamAttrValue              int
#define cudaStreamSetAttribute(s,a,v)    ((void)0)
#define cudaStreamAttributeAccessPolicyWindow 0
#define cudaAccessPropertyPersisting     0
#define cudaAccessPropertyStreaming      0

// __umul64hi is already defined in AMD's amd_device_functions.h.
// No need to define it here — AMD provides it natively.

// __syncwarp(mask): NVIDIA-only warp synchronization primitive.
// On AMD GCN/RDNA all threads in a wavefront execute in lockstep,
// so an explicit warp sync is a no-op.
#define __syncwarp(mask)  ((void)0)
#define __syncwarp()      ((void)0)

#else
// ── NVIDIA CUDA compilation path ─────────────────────────────────────────────
#include <cuda_runtime.h>
#endif  // __HIPCC__

// ============================================================================
// Note: portable carry-chain arithmetic (__uint128_t fallbacks for PTX) is
// implemented directly inside each .cu file (kangaroo_kernel.cu,
// puzzle_optimized.cu, secp256k1.cu) via #if defined(__CUDA_ARCH__) guards.
// It is NOT defined here because unsigned __int128 is unsupported by MSVC
// (the Windows nvcc host compiler), so putting it in a shared header would
// break Windows CUDA builds.
// ============================================================================


