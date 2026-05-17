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

#else
// ── NVIDIA CUDA compilation path ─────────────────────────────────────────────
#include <cuda_runtime.h>
#endif  // __HIPCC__

// ============================================================================
// Portable 256-bit carry-chain arithmetic
//
// The CUDA path uses PTX inline assembly (add.cc / addc / sub.cc / subc)
// which is NVIDIA-specific. HIP and host code use __uint128_t instead.
//
// Only the two functions that are actually called from the kernel need
// this portability layer; higher-level functions (mod_add, mod_sub, etc.)
// are written purely in terms of these two primitives.
// ============================================================================

namespace collider {
namespace gpu {
namespace portable {

// add256_carry: r = a + b (mod 2^256), returns carry (0 or 1)
__device__ __forceinline__
uint64_t add256_carry(uint64_t* __restrict__ r,
                      const uint64_t* __restrict__ a,
                      const uint64_t* __restrict__ b)
{
#if defined(__CUDA_ARCH__)
    // PTX carry-chain: fastest path on NVIDIA
    uint64_t carry;
    asm volatile(
        "add.cc.u64  %0, %5, %9;\n\t"
        "addc.cc.u64 %1, %6, %10;\n\t"
        "addc.cc.u64 %2, %7, %11;\n\t"
        "addc.cc.u64 %3, %8, %12;\n\t"
        "addc.u64    %4, 0, 0;"
        : "=l"(r[0]), "=l"(r[1]), "=l"(r[2]), "=l"(r[3]), "=l"(carry)
        : "l"(a[0]), "l"(a[1]), "l"(a[2]), "l"(a[3]),
          "l"(b[0]), "l"(b[1]), "l"(b[2]), "l"(b[3])
    );
    return carry;
#else
    // Portable path: __uint128_t (HIP device + host)
    typedef unsigned __int128 u128;
    u128 s = (u128)a[0] + b[0];
    r[0] = (uint64_t)s; s >>= 64;
    s += (u128)a[1] + b[1]; r[1] = (uint64_t)s; s >>= 64;
    s += (u128)a[2] + b[2]; r[2] = (uint64_t)s; s >>= 64;
    s += (u128)a[3] + b[3]; r[3] = (uint64_t)s;
    return (uint64_t)(s >> 64);
#endif
}

// sub256_borrow: r = a - b (mod 2^256), returns ~0ULL if borrow, 0 otherwise
__device__ __forceinline__
uint64_t sub256_borrow(uint64_t* __restrict__ r,
                       const uint64_t* __restrict__ a,
                       const uint64_t* __restrict__ b)
{
#if defined(__CUDA_ARCH__)
    uint64_t borrow;
    asm volatile(
        "sub.cc.u64  %0, %5, %9;\n\t"
        "subc.cc.u64 %1, %6, %10;\n\t"
        "subc.cc.u64 %2, %7, %11;\n\t"
        "subc.cc.u64 %3, %8, %12;\n\t"
        "subc.u64    %4, 0, 0;"
        : "=l"(r[0]), "=l"(r[1]), "=l"(r[2]), "=l"(r[3]), "=l"(borrow)
        : "l"(a[0]), "l"(a[1]), "l"(a[2]), "l"(a[3]),
          "l"(b[0]), "l"(b[1]), "l"(b[2]), "l"(b[3])
    );
    return borrow;  // 0xFFFFFFFFFFFFFFFF if borrow, 0 if no borrow (PTX subc behaviour)
#else
    // Two's-complement subtraction via __uint128_t
    // a - b = a + ~b + 1; carry-out 1 means no borrow, 0 means borrow
    typedef unsigned __int128 u128;
    u128 s = (u128)a[0] + ~b[0] + 1u;
    r[0] = (uint64_t)s; uint64_t c = (uint64_t)(s >> 64);
    s = (u128)a[1] + ~b[1] + c; r[1] = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (u128)a[2] + ~b[2] + c; r[2] = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (u128)a[3] + ~b[3] + c; r[3] = (uint64_t)s; c = (uint64_t)(s >> 64);
    // c==1 → no borrow (a >= b); c==0 → borrow (a < b)
    // Match PTX convention: return 0xFFFF... on borrow, 0 on no-borrow
    return c ? 0ULL : ~0ULL;
#endif
}

// Portable per-limb carry-chain helpers (used by puzzle_optimized.cu)
// On CUDA: fused PTX within a single asm block to avoid CC-register hazards.
// On HIP/host: __uint128_t.

__device__ __forceinline__
uint64_t add_cc(uint64_t a, uint64_t b, uint64_t& carry_out)
{
#if defined(__CUDA_ARCH__)
    uint64_t r;
    asm volatile("add.cc.u64 %0, %2, %3;\n\t addc.u64 %1, 0, 0;"
                 : "=l"(r), "=l"(carry_out) : "l"(a), "l"(b));
    return r;
#else
    typedef unsigned __int128 u128;
    u128 s = (u128)a + b;
    carry_out = (uint64_t)(s >> 64);
    return (uint64_t)s;
#endif
}

__device__ __forceinline__
uint64_t addc_cc(uint64_t a, uint64_t b, uint64_t carry_in, uint64_t& carry_out)
{
#if defined(__CUDA_ARCH__)
    uint64_t r;
    asm volatile("add.cc.u64 %0, %2, %3;\n\t addc.u64 %1, 0, 0;"
                 : "=l"(r), "=l"(carry_out) : "l"(a), "l"(b));
    if (carry_in) { if (r == UINT64_MAX) carry_out = 1; r++; }
    return r;
#else
    typedef unsigned __int128 u128;
    u128 s = (u128)a + b + (carry_in & 1u);
    carry_out = (uint64_t)(s >> 64);
    return (uint64_t)s;
#endif
}

__device__ __forceinline__
uint64_t sub_cc(uint64_t a, uint64_t b, uint64_t& borrow_out)
{
#if defined(__CUDA_ARCH__)
    uint64_t r;
    asm volatile("sub.cc.u64 %0, %2, %3;\n\t subc.u64 %1, 0, 0;"
                 : "=l"(r), "=l"(borrow_out) : "l"(a), "l"(b));
    return r;
#else
    borrow_out = (a < b) ? UINT64_MAX : 0ULL;
    return a - b;
#endif
}

__device__ __forceinline__
uint64_t subc_cc(uint64_t a, uint64_t b, uint64_t borrow_in, uint64_t& borrow_out)
{
#if defined(__CUDA_ARCH__)
    uint64_t b1 = borrow_in >> 63;
    uint64_t r;
    asm volatile("sub.cc.u64 %0, %2, %3;\n\t subc.u64 %1, 0, 0;"
                 : "=l"(r), "=l"(borrow_out) : "l"(a), "l"(b));
    if (b1) { if (r == 0) borrow_out = UINT64_MAX; r--; }
    return r;
#else
    uint64_t b1 = (borrow_in >> 63) & 1u;
    typedef unsigned __int128 u128;
    // a - b - b1 via two's-complement trick
    u128 s = (u128)a + ~b + 1u;
    uint64_t r0 = (uint64_t)s;
    uint64_t c0 = (uint64_t)(s >> 64);  // 1 if a >= b
    // subtract additional borrow
    uint64_t r1 = r0 - b1;
    uint64_t c1 = (b1 && r0 == 0) ? 1u : 0u;
    uint64_t no_borrow = c0 - c1;
    borrow_out = no_borrow ? 0ULL : UINT64_MAX;
    return r1;
#endif
}

}  // namespace portable
}  // namespace gpu
}  // namespace collider
