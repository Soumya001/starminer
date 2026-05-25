/**
 * Minimal secp256k1 device API — types and extern __device__ declarations.
 *
 * Include from .cu files that need to call ec_mul_simple via CUDA/HIP
 * separable compilation. Types MUST match secp256k1.cu exactly (same
 * namespace, same layout) so nvlink resolves the cross-TU device call.
 *
 * Do NOT include from .cpp files compiled by MSVC — __device__ is nvcc/hipcc
 * specific. Guard the include with #ifdef __CUDACC__ or #ifdef __HIPCC__
 * if there is any risk of MSVC seeing this header.
 */

#pragma once
#include <cstdint>

// Types declared in namespace starminer::gpu to match secp256k1.cu exactly.
// The linker (nvlink/hipld) uses the C++ mangled name which encodes the
// namespace; a namespace mismatch causes "undefined device function" at link.

namespace starminer {
namespace gpu {

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

// ec_mul_simple: scalar * G on secp256k1 in affine coordinates.
// Defined in src/gpu/secp256k1.cu; accessible here via separable compilation.
extern __device__ void ec_mul_simple(ECPointAffine& result, const uint256& scalar);

}  // namespace gpu
}  // namespace starminer
