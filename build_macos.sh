#!/bin/bash
# Build starminer-pro on macOS (Apple Silicon / Metal)
set -e

cd "$(dirname "$0")"

echo "=== Pulling latest ==="
git pull origin develop

echo "=== Configuring CMake (Metal) ==="
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTARMINER_EDITION_PRO=ON \
  -DSTARMINER_USE_METAL=ON \
  -DSTARMINER_USE_CUDA=OFF \
  -DSTARMINER_BUILD_TESTS=OFF \
  -DSTARMINER_BUILD_BENCHMARKS=OFF \
  -DSTARMINER_BUILD_TOOLS=OFF

echo "=== Building ==="
cmake --build build --parallel $(sysctl -n hw.ncpu)

if [ -f build/starminer_pro ]; then
  echo "=== SUCCESS ==="
  ls -lh build/starminer_pro
  file build/starminer_pro
else
  echo "=== FAILED ==="
  exit 1
fi
