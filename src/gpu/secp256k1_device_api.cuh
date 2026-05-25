/**
 * Minimal secp256k1 device API — types and extern __device__ declarations.
 *
 * Include this from any .cu that needs to call ec_mul_simple or ec_add_mixed
 * via CUDA separable compilation. The real implementations live in
 * src/gpu/secp256k1.cu; they are linked into the same executable via
 * CUDA_SEPARABLE_COMPILATION ON in CMakeLists.txt.
 *
 * Do NOT place this in a shared header that MSVC or a non-CUDA compiler sees
 * — unsigned __int128 and __device__ are nvcc/hipcc-only.
 */

#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Types (must match secp256k1.cu exactly so the linker resolves symbols).
// ---------------------------------------------------------------------------

struct uint256 {
    uint32_t limbs[8];

    __device__ __forceinline__ bool is_zero() const {
        return (limbs[0] | limbs[1] | limbs[2] | limbs[3] |
                limbs[4] | limbs[5] | limbs[6] | limbs[7]) == 0;
    }
    __device__ __forceinline__ void set_zero() {
        #pragma unroll
        for (int i = 0; i < 8; i++) limbs[i] = 0;
    }
    __device__ __forceinline__ void set_one() {
        limbs[0] = 1;
        #pragma unroll
        for (int i = 1; i < 8; i++) limbs[i] = 0;
    }
};

struct ECPointJacobian {
    uint256 X, Y, Z;
    __device__ __forceinline__ bool is_infinity() const { return Z.is_zero(); }
    __device__ __forceinline__ void set_infinity() {
        X.set_one(); Y.set_one(); Z.set_zero();
    }
};

struct ECPointAffine {
    uint256 x;
    uint256 y;
};

// ---------------------------------------------------------------------------
// Cross-TU __device__ declarations.
// These call through separable-compilation to secp256k1.cu.
// ---------------------------------------------------------------------------
extern __device__ void ec_mul_simple(ECPointAffine& result, const uint256& scalar);
