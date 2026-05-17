#!/bin/bash
# build.sh - Build StarMiner with CMake 4.x + CUDA 12 workaround
# Fixes: nvlink fatal elfLink linker library load error with empty libdl.a/librt.a stubs
#        and auto-enabled -dlto on CMake 4.x

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Detect GPU architecture for optimal codegen
GPU_ARCH=""
if command -v nvidia-smi &> /dev/null; then
    # Try to detect compute capability
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -n1)
    case "$GPU_NAME" in
        *"RTX 3060"*|*"RTX 3070"*|*"RTX 3080"*|*"RTX 3090"*|*"RTX 3060 Ti"*|*"RTX 3070 Ti"*|*"RTX 3080 Ti"*)
            GPU_ARCH="86"
            ;;
        *"RTX 4090"*|*"RTX 4080"*|*"RTX 4070"*)
            GPU_ARCH="89"
            ;;
        *"A100"*)
            GPU_ARCH="80"
            ;;
        *)
            echo "Unknown GPU: $GPU_NAME, using native arch"
            GPU_ARCH="native"
            ;;
    esac
    echo "Detected GPU: $GPU_NAME -> arch=sm_$GPU_ARCH"
else
    GPU_ARCH="native"
    echo "nvidia-smi not found, using native arch"
fi

# Clean and configure
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES="$GPU_ARCH"

# Workaround: CMake 4.x auto-adds -dlto and pulls empty libdl.a/librt.a stubs into
# nvlink's device link step, causing "elfLink linker library load error" on
# Ubuntu 24.04 with glibc 2.34+ (empty static stubs).
echo "Applying CMake 4.x + CUDA 12 nvlink workarounds..."

# Remove -dlto from device link commands
find "$BUILD_DIR" -name "dlink.txt" -exec sed -i 's/ -dlto / /g' {} +

# Remove empty static host library stubs from device link libs
find "$BUILD_DIR" -name "deviceLinkLibs.rsp" -exec sed -i 's/-ldl //g; s|"/usr/lib/x86_64-linux-gnu/librt.a" ||g' {} +

# Build
make -j$(nproc) starminer

echo ""
echo "Build complete: ${BUILD_DIR}/starminer"
