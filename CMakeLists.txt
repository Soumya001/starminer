cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Cross-Platform GPU Compute Project
# Supports: CUDA (Windows/Linux), Metal (macOS), CPU fallback
# ============================================================================

# ============================================================================
# vcpkg Auto-Bootstrap (Windows only - downloads dependencies automatically)
# ============================================================================
if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    # Check if vcpkg is already installed
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
        message(STATUS "Using existing vcpkg: $ENV{VCPKG_ROOT}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
        message(STATUS "Using local vcpkg")
    else()
        # Auto-download vcpkg
        message(STATUS "Downloading vcpkg for automatic dependency management...")
        include(FetchContent)
        FetchContent_Declare(
            vcpkg
            GIT_REPOSITORY https://github.com/microsoft/vcpkg.git
            GIT_TAG 2024.01.12
            GIT_SHALLOW TRUE
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/vcpkg
        )
        FetchContent_MakeAvailable(vcpkg)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

        # Bootstrap vcpkg
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/vcpkg.exe")
            message(STATUS "Bootstrapping vcpkg...")
            execute_process(
                COMMAND cmd /c "${CMAKE_SOURCE_DIR}/vcpkg/bootstrap-vcpkg.bat" -disableMetrics
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/vcpkg
                RESULT_VARIABLE VCPKG_BOOTSTRAP_RESULT
            )
            if(NOT VCPKG_BOOTSTRAP_RESULT EQUAL 0)
                message(WARNING "vcpkg bootstrap failed - OpenSSL may not be available")
            endif()
        endif()
    endif()

    # Set triplet for x64 Windows — static so no DLLs (libcurl/openssl) are needed at runtime
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
endif()

# Detect platform before project declaration.
# STARMINER_USE_CUDA can be forced OFF via -DSTARMINER_USE_CUDA=OFF before
# option() is parsed, so check it here to avoid nvcc detection on CPU builds.
if(NOT DEFINED STARMINER_USE_CUDA)
    set(STARMINER_USE_CUDA ON)
endif()

if(APPLE)
    set(STARMINER_PLATFORM_MACOS TRUE)
    project(starminer LANGUAGES CXX OBJCXX)
elseif(WIN32)
    set(STARMINER_PLATFORM_WINDOWS TRUE)
    if(STARMINER_USE_CUDA)
        project(starminer LANGUAGES CXX CUDA)
    else()
        project(starminer LANGUAGES CXX)
    endif()
else()
    set(STARMINER_PLATFORM_LINUX TRUE)
    if(STARMINER_USE_CUDA)
        project(starminer LANGUAGES CXX CUDA)
    else()
        project(starminer LANGUAGES CXX)
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime on Windows so users don't need VC++ Redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ============================================================================
# Options
# ============================================================================
option(STARMINER_USE_CUDA "Enable CUDA backend" ON)
option(STARMINER_USE_ROCM "Enable AMD ROCm/HIP backend" OFF)
option(STARMINER_USE_METAL "Enable Metal backend" ON)
option(STARMINER_USE_CPU "Enable CPU fallback backend" ON)
option(STARMINER_BUILD_TESTS "Build tests" ON)
option(STARMINER_BUILD_BENCHMARKS "Build benchmarks" ON)
option(STARMINER_BUILD_TOOLS "Build CLI tools" ON)

message(STATUS "Building StarMiner")

# ============================================================================
# Backend Detection
# ============================================================================
set(STARMINER_BACKEND "CPU")  # Default

if(APPLE AND STARMINER_USE_METAL)
    # macOS: Use Metal
    set(STARMINER_BACKEND "METAL")
    add_compile_definitions(STARMINER_USE_METAL=1)
    add_compile_definitions(STARMINER_PLATFORM_MACOS=1)

    # Check for Apple Silicon
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(ARCH STREQUAL "arm64")
        add_compile_definitions(STARMINER_APPLE_SILICON=1)
        message(STATUS "Detected Apple Silicon (${ARCH})")
    endif()

elseif(STARMINER_USE_CUDA)
    # Windows/Linux: Try CUDA
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(STARMINER_BACKEND "CUDA")
        enable_language(CUDA)
        add_compile_definitions(STARMINER_USE_CUDA=1)

        set(CMAKE_CUDA_STANDARD 20)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)

        # Detect GPU architecture
        # RTX 5090 (Blackwell): SM 10.0
        # RTX 4090 (Ada): SM 8.9
        # RTX 3060/3070/3080/3090 (Ampere): SM 8.6
        # We compile for all to support various cards
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            # Check if we can detect the GPU
            set(CMAKE_CUDA_ARCHITECTURES "86;89;100")
            message(STATUS "Compiling for CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
        endif()

        # Static CUDA runtime so users don't need cudart64_12.dll installed
        if(WIN32)
            set(CMAKE_CUDA_RUNTIME_LIBRARY Static)
        else()
            set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
        endif()

        # CUDA optimization flags - aggressive optimization for compute-bound kernels
        set(CMAKE_CUDA_FLAGS_RELEASE "-O3 --use_fast_math -DNDEBUG")
        # Additional CUDA optimizations for high-performance computing
        set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} --extra-device-vectorization")
        set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} --restrict")
        set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} --fmad=true")
        # Lineinfo for profiling (minimal overhead, enables nsys/ncu profiling)
        set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} -lineinfo")
        # Suppress deprecated GPU architecture warnings (for older GPUs like GTX 9xx)
        set(CMAKE_CUDA_FLAGS_RELEASE "${CMAKE_CUDA_FLAGS_RELEASE} -Wno-deprecated-gpu-targets")
        # Phase 5+ kernels (v1.4.0) use lambdas + relaxed-constexpr inside __device__
        # functions. Apply to all configurations, not just Release.
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --extended-lambda --expt-relaxed-constexpr")

        # Debug/Profile build for detailed profiling
        set(CMAKE_CUDA_FLAGS_RELWITHDEBINFO "-O2 -g -G -lineinfo --use_fast_math")

        if(WIN32)
            add_compile_definitions(STARMINER_PLATFORM_WINDOWS=1)
            add_compile_definitions(NOMINMAX)           # Prevent Windows min/max macro conflicts
            add_compile_definitions(WIN32_LEAN_AND_MEAN) # Reduce Windows header bloat
            # Use static cudart on Windows to avoid double-linking:
            # CMAKE_CUDA_RUNTIME_LIBRARY=Static makes device objects use cudart_static.lib,
            # so we must also link CUDA::cudart_static (not CUDA::cudart) at the target level.
            set(CUDA_RT_TARGET CUDA::cudart_static)
        else()
            add_compile_definitions(STARMINER_PLATFORM_LINUX=1)
            set(CUDA_RT_TARGET CUDA::cudart)
        endif()
    endif()
elseif(STARMINER_USE_ROCM)
    # Linux/Windows: Try AMD ROCm / HIP
    # On Windows, set CMAKE_PREFIX_PATH to AMD HIP SDK install dir before configuring
    find_package(hip QUIET)
    if(hip_FOUND)
        set(STARMINER_BACKEND "ROCM")
        enable_language(HIP)
        add_compile_definitions(STARMINER_USE_ROCM=1)
        if(UNIX)
            add_compile_definitions(STARMINER_PLATFORM_LINUX=1)
        endif()

        # RDNA2 (RX 6xxx) = gfx1030, RDNA3 (RX 7xxx) = gfx1100-1102
        # RDNA1 (RX 5xxx) = gfx1010, Vega = gfx906/gfx908
        if(NOT DEFINED CMAKE_HIP_ARCHITECTURES)
            set(CMAKE_HIP_ARCHITECTURES "gfx906;gfx908;gfx1010;gfx1030;gfx1100;gfx1101;gfx1102")
            message(STATUS "Compiling for HIP architectures: ${CMAKE_HIP_ARCHITECTURES}")
        endif()

        if(MSVC)
            set(CMAKE_HIP_FLAGS_RELEASE "-O3 -DNDEBUG")
        else()
            set(CMAKE_HIP_FLAGS_RELEASE "-O3 -DNDEBUG")
        endif()
        message(STATUS "AMD ROCm/HIP backend enabled (${CMAKE_SYSTEM_NAME})")
    else()
        message(WARNING "STARMINER_USE_ROCM=ON but HIP not found — falling back to CPU")
    endif()
endif()

if(STARMINER_BACKEND STREQUAL "CPU")
    add_compile_definitions(STARMINER_USE_CPU=1)
    message(STATUS "Using CPU fallback backend")
endif()

message(STATUS "StarMiner backend: ${STARMINER_BACKEND}")

# ============================================================================
# Optimization Flags
# ============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(APPLE)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
        if(ARCH STREQUAL "arm64")
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mcpu=apple-m1")
        endif()
    elseif(MSVC)
        set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG /arch:AVX2 /GL /EHsc")
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG")
        set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "/LTCG")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    endif()
endif()

# ============================================================================
# Link-Time Optimization (LTO/IPO) - 5-15% performance boost
# ============================================================================
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

if(IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT STARMINER_DISABLE_IPO
   AND NOT STARMINER_BACKEND STREQUAL "CUDA")
    # Note: CUDA builds skip global IPO entirely. Setting
    # CMAKE_INTERPROCEDURAL_OPTIMIZATION=TRUE compiles starminer_gpu objects
    # as device LTO bitcode; the final device link step then requires -dlto
    # across 8 GPU architectures, which OOM-kills the 7 GB CI runner.
    # CMAKE_CUDA_INTERPROCEDURAL_OPTIMIZATION=FALSE does not reliably suppress
    # this — the only safe fix is to not enable global IPO with CUDA at all.
    message(STATUS "LTO/IPO enabled for Release builds (host C/C++ only)")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    if(STARMINER_BACKEND STREQUAL "CUDA")
        message(STATUS "LTO/IPO disabled: CUDA build — device-link -dlto OOM-kills CI runner on 8-arch builds")
    elseif(STARMINER_DISABLE_IPO)
        message(STATUS "LTO/IPO disabled by STARMINER_DISABLE_IPO=ON")
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "LTO/IPO disabled (not Release build)")
    else()
        message(WARNING "LTO/IPO not supported: ${IPO_ERROR}")
    endif()
endif()

# ============================================================================
# Find Dependencies
# ============================================================================
find_package(Threads REQUIRED)

# OpenSSL - help CMake find OpenSSL on various platforms
if(APPLE)
    # Apple Silicon Homebrew path
    if(EXISTS "/opt/homebrew/opt/openssl")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
    # Intel Mac Homebrew path
    elseif(EXISTS "/usr/local/opt/openssl")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
    endif()
elseif(WIN32)
    # Common Windows OpenSSL installation paths
    set(_WIN_OPENSSL_PATHS
        "C:/Program Files/OpenSSL-Win64"
        "C:/Program Files/OpenSSL"
        "C:/OpenSSL-Win64"
        "C:/OpenSSL"
        "$ENV{VCPKG_ROOT}/installed/x64-windows"
    )
    foreach(_PATH ${_WIN_OPENSSL_PATHS})
        if(EXISTS "${_PATH}")
            set(OPENSSL_ROOT_DIR "${_PATH}")
            break()
        endif()
    endforeach()
endif()

find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found - TLS support enabled")
    message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
    message(STATUS "  OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
else()
    message(STATUS "OpenSSL not found - TLS support disabled")
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "libcurl found - license validation enabled")
else()
    message(STATUS "libcurl not found - license validation will be disabled")
endif()

# XXH3 - SIMD-optimized hash for bloom filter (header-only)
include(FetchContent)
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.2
)
FetchContent_MakeAvailable(xxhash)
# xxHash is header-only, just need include path
set(XXHASH_INCLUDE_DIR ${xxhash_SOURCE_DIR})

if(APPLE AND STARMINER_BACKEND STREQUAL "METAL")
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(COCOA_LIBRARY Cocoa)
endif()

# ============================================================================
# Include Directories
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/src)

# ============================================================================
# Platform Abstraction Library
# ============================================================================
set(PLATFORM_SOURCES
    src/platform/platform.hpp
)

if(STARMINER_BACKEND STREQUAL "CUDA")
    list(APPEND PLATFORM_SOURCES src/platform/cuda_platform.cu)
    list(APPEND PLATFORM_SOURCES src/platform/nvml_wrapper.cpp)
elseif(STARMINER_BACKEND STREQUAL "ROCM")
    list(APPEND PLATFORM_SOURCES src/platform/cuda_platform.cu)
    set_source_files_properties(src/platform/cuda_platform.cu PROPERTIES LANGUAGE HIP)
elseif(STARMINER_BACKEND STREQUAL "METAL")
    list(APPEND PLATFORM_SOURCES src/platform/metal_platform.mm)
else()
    list(APPEND PLATFORM_SOURCES src/platform/cpu_platform.cpp)
endif()

if(STARMINER_BACKEND STREQUAL "METAL")
    add_library(starminer_platform STATIC ${PLATFORM_SOURCES})
    target_link_libraries(starminer_platform
        ${METAL_LIBRARY}
        ${FOUNDATION_LIBRARY}
        Threads::Threads
    )
    set_source_files_properties(src/platform/metal_platform.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
elseif(STARMINER_BACKEND STREQUAL "CUDA")
    add_library(starminer_platform STATIC ${PLATFORM_SOURCES})
    target_link_libraries(starminer_platform
        ${CUDA_RT_TARGET}
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    set_target_properties(starminer_platform PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
        POSITION_INDEPENDENT_CODE ON
    )
elseif(STARMINER_BACKEND STREQUAL "ROCM")
    add_library(starminer_platform STATIC ${PLATFORM_SOURCES})
    target_link_libraries(starminer_platform hip::host Threads::Threads)
    set_target_properties(starminer_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
    add_library(starminer_platform STATIC ${PLATFORM_SOURCES})
    target_link_libraries(starminer_platform
        Threads::Threads
    )
endif()

# ============================================================================
# Core Library (CPU-side intelligence layer)
# ============================================================================
set(CORE_SOURCES
    src/core/rule_engine.cpp
    src/core/cpu_kangaroo_backend.cpp
    src/core/kangaroo_backend_factory.cpp
    src/cli/cli_parser.cpp
    # v1.4.1 A.3: top-level interactive flow (banner + main menu + per-mode
    # submenus + Windows ANSI enabler) extracted from main.cpp. The brain
    # the file itself compiles in both editions.
    src/ui/interactive_ui.cpp
    # v1.4.1 A.3: pool-mode runtime driver extracted from main.cpp. Free +
    # Pro both build it; the bloom-filter feature inside is #ifdef-gated.
    src/runtime/pool_solver.cpp
    # v1.4.1 A.3 (5/6): puzzle-mode + benchmark runtime drivers extracted
    # from main.cpp. Available in BOTH Free and Pro builds; the brain-wallet
    # site (Pro-only includes from gpu/brain_wallet_gpu.hpp). The Free path
    # uses the CPU + Metal/CUDA SHA-256 benchmark with no Pro symbols.
    src/runtime/puzzle_solver.cpp
    # v1.4.1 A.3 (6/6): GPU detection helper extracted from main.cpp so
    # the dispatcher (main.cpp) and the runtime drivers can both link
    # against detect_gpus() without main.cpp owning the body.
    src/runtime/gpu_detection.cpp
    # v1.4.1 UI: live BTC balance lookup against mempool.space for the
    # solved-puzzle banner. libcurl-gated -- if CURL_FOUND was false at
    # configure time, the function returns nullopt and the banner shows
    # "balance unavailable (offline)".
    src/ui/btc_balance.cpp
    # Background GitHub Releases version check. libcurl-gated: when curl
    # is absent, fetch_latest_version() returns "" and the check is skipped.
    src/core/update_checker.cpp
)

# Free-licensed Metal SHA-256 benchmark (Mac only). Available in BOTH
# Free and Pro builds so `--benchmark` can validate the GPU on Metal.
if(APPLE AND STARMINER_USE_METAL
   AND EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/sha256_metal_bench.mm")
    list(APPEND CORE_SOURCES src/gpu/sha256_metal_bench.mm)
    set_source_files_properties(src/gpu/sha256_metal_bench.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

# Metal Kangaroo solver (Phase 1 of Mac pool support, v1.4.0). Free-
# licensed; available in BOTH Free and Pro Mac builds so pool mining
# on Apple Silicon stops falling back to CPU kangaroo at ~200 K/s.
if(APPLE AND STARMINER_USE_METAL
   AND EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/kangaroo_metal.mm")
    list(APPEND CORE_SOURCES src/gpu/kangaroo_metal.mm)
    set_source_files_properties(src/gpu/kangaroo_metal.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    # Metal IKangarooBackend adapter -- gated to the same conditions as
    # the underlying KangarooMetalSolver above.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/metal_kangaroo_backend.mm")
        list(APPEND CORE_SOURCES src/gpu/metal_kangaroo_backend.mm)
        set_source_files_properties(src/gpu/metal_kangaroo_backend.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    # v1.4.1: Metal-backed MultiGPUKangarooManager for standalone puzzle
    # solving. Pre-1.4.1 the standalone path's MultiGPU class was a
    # no-CUDA stub on Mac that returned false from init() and silently
    # fell back to CPU kangaroo. This file implements the full surface
    # on top of KangarooMetalSolver with host-side collision detection.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/metal_multi_gpu_kangaroo.mm")
        list(APPEND CORE_SOURCES src/gpu/metal_multi_gpu_kangaroo.mm)
        set_source_files_properties(src/gpu/metal_multi_gpu_kangaroo.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    # v1.4.1: Metal-backed brute-force puzzle solver. Pre-1.4.1 the Mac
    # build of `starminer --puzzle N` (no --kangaroo) hit the no-CUDA stub
    # of MultiGPUPuzzleSolver and silently fell back to the CPU reference
    # at ~30 KKeys/s. puzzle_metal.mm is the per-device dispatcher;
    # metal_multi_gpu_puzzle.mm bridges it to the MultiGPUPuzzleSolver
    # surface that puzzle_solver.cpp consumes.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/puzzle_metal.mm")
        list(APPEND CORE_SOURCES src/gpu/puzzle_metal.mm)
        set_source_files_properties(src/gpu/puzzle_metal.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/metal_multi_gpu_puzzle.mm")
        list(APPEND CORE_SOURCES src/gpu/metal_multi_gpu_puzzle.mm)
        set_source_files_properties(src/gpu/metal_multi_gpu_puzzle.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
    # Bake the MSL source into a generated C++ header so neither the
    # production dispatcher nor the test binaries need to search the
    # filesystem for kangaroo.metal at runtime. The previous approach
    # (configure_file COPYONLY + dual-loader candidate-path lists in
    # kangaroo_metal.mm and tests/test_metal_secp256k1.mm) had three
    # problems: the candidate lists drifted; running the binary from
    # an unexpected cwd produced "shader not found"; and the staged
    # build/ copy could go stale on parallel builds.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/kangaroo.metal")
        set(_kangaroo_metal_src "${CMAKE_SOURCE_DIR}/src/gpu/kangaroo.metal")
        set(_kangaroo_metal_hdr "${CMAKE_BINARY_DIR}/generated/kangaroo_metal_source.h")
        file(READ "${_kangaroo_metal_src}" KANGAROO_METAL_SOURCE_BODY)
        # Hand the source to the compiler as a raw string literal so
        # quotes / backslashes / newlines pass through untouched. The
        # delimiter `KANGAROO_METAL` must not appear in the .metal text
        # itself; trivially true today.
        configure_file(
            "${CMAKE_SOURCE_DIR}/cmake/kangaroo_metal_source.h.in"
            "${_kangaroo_metal_hdr}"
            @ONLY)
        # Re-run the configure step whenever the .metal file changes so
        # the generated header stays in sync. CMake doesn't track
        # `file(READ)` inputs automatically.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${_kangaroo_metal_src}")
        # Expose the generated/ directory to anything that needs to
        # `#include "kangaroo_metal_source.h"`. Production code lives in
        # starminer_core (added below); the test target adds the same
        # directory directly in its own block.
        set(KANGAROO_METAL_GENERATED_INCLUDE_DIR
            "${CMAKE_BINARY_DIR}/generated"
            CACHE INTERNAL "Generated kangaroo MSL header dir")
    endif()
    # Same embedding pattern for the puzzle MSL source. The .h.in template
    # at cmake/puzzle_metal_source.h.in is a near-clone of the kangaroo
    # one; intentionally separate so a future refactor can change one
    # without touching the other.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/puzzle.metal")
        set(_puzzle_metal_src "${CMAKE_SOURCE_DIR}/src/gpu/puzzle.metal")
        set(_puzzle_metal_hdr "${CMAKE_BINARY_DIR}/generated/puzzle_metal_source.h")
        file(READ "${_puzzle_metal_src}" PUZZLE_METAL_SOURCE_BODY)
        configure_file(
            "${CMAKE_SOURCE_DIR}/cmake/puzzle_metal_source.h.in"
            "${_puzzle_metal_hdr}"
            @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${_puzzle_metal_src}")
    endif()
endif()

# Brain-wallet host-side sources.
# brain_wallet_gpu.cu is CUDA-only (added to GPU_SOURCES below).
if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/gpu_rules.cpp"
   AND NOT STARMINER_BACKEND STREQUAL "CUDA")
    list(APPEND CORE_SOURCES src/gpu/gpu_rules.cpp)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/v2/v2_orchestrator.cpp")
    list(APPEND CORE_SOURCES src/gpu/v2/v2_orchestrator.cpp)
endif()
if(APPLE AND STARMINER_USE_METAL AND
   EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/v2/v2_metal_dispatch.mm")
    list(APPEND CORE_SOURCES src/gpu/v2/v2_metal_dispatch.mm)
    set_source_files_properties(src/gpu/v2/v2_metal_dispatch.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc -fobjc-weak")
endif()

# Check if source files exist
foreach(SRC ${CORE_SOURCES})
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${SRC}")
        list(REMOVE_ITEM CORE_SOURCES ${SRC})
    endif()
endforeach()

if(CORE_SOURCES)
    add_library(starminer_core STATIC ${CORE_SOURCES})
    target_link_libraries(starminer_core
        starminer_platform
        Threads::Threads
    )
    target_include_directories(starminer_core PUBLIC ${XXHASH_INCLUDE_DIR})
    # v1.4.1 UI: btc_balance.cpp uses libcurl when available. Define
    # STARMINER_HAVE_CURL=1 so the header's #ifdef chooses the live
    # mempool.space code path; otherwise the stub returns nullopt.
    if(CURL_FOUND)
        target_link_libraries(starminer_core CURL::libcurl)
        target_compile_definitions(starminer_core PRIVATE STARMINER_HAVE_CURL=1)
    endif()
    if(KANGAROO_METAL_GENERATED_INCLUDE_DIR)
        target_include_directories(starminer_core PUBLIC
            ${KANGAROO_METAL_GENERATED_INCLUDE_DIR})
    endif()
    # Phase 11: link Metal + Foundation when v2_metal_dispatch.mm is in the build.
    if(APPLE AND STARMINER_USE_METAL)
        target_link_libraries(starminer_core
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY}
        )
    endif()
else()
    # Create empty library if no sources exist yet
    add_library(starminer_core INTERFACE)
    target_link_libraries(starminer_core INTERFACE
        starminer_platform
        Threads::Threads
    )
    target_include_directories(starminer_core INTERFACE ${XXHASH_INCLUDE_DIR})
endif()

# ============================================================================
# JLP Protocol Codegen
# ============================================================================
# The C++ wire format header is generated from protocol/jlp.yaml by
# tools/codegen/jlp_codegen.py. The generated header lives at
# src/pool/jlp_wire_generated.hpp and is checked into the repo (so Free
# builds, which lack the codegen tool, can still compile). Pro builds
# regenerate it on every configure to catch IDL drift early.
set(JLP_IDL "${CMAKE_SOURCE_DIR}/protocol/jlp.yaml")
set(JLP_GEN_CPP "${CMAKE_SOURCE_DIR}/src/pool/jlp_wire_generated.hpp")
set(JLP_GEN_PY  "${CMAKE_SOURCE_DIR}/data/protocol/jlp_protocol_generated.py")
set(JLP_CODEGEN "${CMAKE_SOURCE_DIR}/tools/codegen/jlp_codegen.py")

if(EXISTS "${JLP_IDL}" AND EXISTS "${JLP_CODEGEN}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        # Drift check at configure time: warn but don't fail (the
        # generated artifacts are committed; CI re-runs --check).
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${JLP_CODEGEN} --check
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE JLP_CHECK_RC
            OUTPUT_VARIABLE JLP_CHECK_OUT
            ERROR_VARIABLE  JLP_CHECK_ERR
        )
        if(NOT JLP_CHECK_RC EQUAL 0)
            message(WARNING
                "JLP codegen drift: regenerate via "
                "`python tools/codegen/jlp_codegen.py`. "
                "stderr: ${JLP_CHECK_ERR}")
        endif()

        # Per-build regen rule (only fires when the IDL or codegen change).
        add_custom_command(
            OUTPUT ${JLP_GEN_CPP} ${JLP_GEN_PY}
            COMMAND ${Python3_EXECUTABLE} ${JLP_CODEGEN}
            DEPENDS ${JLP_IDL} ${JLP_CODEGEN}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Regenerating JLP wire format from protocol/jlp.yaml"
            VERBATIM
        )
        add_custom_target(jlp_codegen DEPENDS ${JLP_GEN_CPP} ${JLP_GEN_PY})
    else()
        message(STATUS "Python3 not found; skipping JLP codegen (using committed generated files)")
    endif()
endif()

# ============================================================================
# Pool Client Library (Network-based distributed solving)
# ============================================================================
set(POOL_SOURCES
    src/pool/jlp_pool_client.cpp
    src/pool/pool_manager.cpp
)

# Check if source files exist
set(EXISTING_POOL_SOURCES)
foreach(SRC ${POOL_SOURCES})
    if(EXISTS "${CMAKE_SOURCE_DIR}/${SRC}")
        list(APPEND EXISTING_POOL_SOURCES ${SRC})
    endif()
endforeach()

if(EXISTING_POOL_SOURCES)
    add_library(starminer_pool STATIC ${EXISTING_POOL_SOURCES})
    if(TARGET jlp_codegen)
        add_dependencies(starminer_pool jlp_codegen)
    endif()
    target_link_libraries(starminer_pool
        Threads::Threads
    )
    target_include_directories(starminer_pool PUBLIC
        ${CMAKE_SOURCE_DIR}/src/pool
    )

    # Windows socket + crypto libs (ws2_32 for sockets, crypt32 for cert store bridge)
    if(WIN32)
        target_link_libraries(starminer_pool ws2_32 crypt32)
    endif()

    # OpenSSL for TLS support
    if(OPENSSL_FOUND)
        target_link_libraries(starminer_pool OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(starminer_pool PUBLIC STARMINER_HAS_OPENSSL)
    endif()

    # libcurl for HTTP pool support
    if(CURL_FOUND)
        target_link_libraries(starminer_pool CURL::libcurl)
        target_compile_definitions(starminer_pool PUBLIC STARMINER_HAVE_CURL=1)
    endif()

    message(STATUS "Pool client library enabled")
endif()


# ============================================================================
# RCKangaroo - High-performance Kangaroo solver (GPLv3, RetiredCoder)
# ============================================================================
if(STARMINER_BACKEND STREQUAL "CUDA")
    option(STARMINER_USE_RCKANGAROO "Use RCKangaroo as Kangaroo solver backend" ON)
    if(STARMINER_USE_RCKANGAROO AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/RCKangaroo")
        add_subdirectory(third_party/RCKangaroo)
        add_compile_definitions(STARMINER_USE_RCKANGAROO=1)
        message(STATUS "RCKangaroo enabled - high-performance Kangaroo solver")
    endif()
endif()

# ============================================================================
# GPU Kernels
# ============================================================================
if(STARMINER_BACKEND STREQUAL "CUDA")
    set(GPU_SOURCES
        src/gpu/sha256.cu
        src/gpu/secp256k1.cu
        src/gpu/ripemd160.cu
        # v1.4.1 BW-DEDUP-3: bloom_filter.cu (impl A, double-hash on
        # raw bytes) was deleted in favour of h160_bloom_filter.cu
        # (impl B, MurmurHash3) which matches build_bloom's .blf
        # format. Bench picked B-texture as winner (3.29x on Ampere
        # 3060). See docs/v1.4.1-mod-inv-audit.md for the parallel
        # decision on mod_inv (kept multiple impls there because of
        # algorithmic divergence; bloom impls were genuinely
        # interchangeable so consolidation made sense).
        src/gpu/h160_bloom_filter.cu
        src/gpu/puzzle_gpu.cu
        src/gpu/puzzle_optimized.cu
        src/gpu/kangaroo_kernel.cu
        # MultiGPUBrainWallet host-API implementation (pimpl body, CUDA-only)
        src/gpu/brain_wallet_gpu.cu
    )

    # Brain Wallet v2 unity TU (all v2 kernels in one TU so nvlink has
    # a single device-symbol graph).
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/v2/v2_kernels_unity.cu" AND
       NOT STARMINER_DISABLE_V2)
        list(APPEND GPU_SOURCES src/gpu/v2/v2_kernels_unity.cu)
    endif()

    # NOTE: src/gpu/v2/v2_orchestrator.cpp is compiled into starminer_core,
    # not starminer_gpu, so main.cpp can call it without pulling in CUDA on
    # Free builds. See the CORE_SOURCES list above.

    # Add RCKangaroo wrapper if enabled
    if(STARMINER_USE_RCKANGAROO AND TARGET rckangaroo)
        list(APPEND GPU_SOURCES src/gpu/rckangaroo_wrapper.cu)
        # CUDA IKangarooBackend adapter -- only compiled with RCKangaroo
        # active. Lives in starminer_gpu (rather than starminer_core)
        # because it #include's rckangaroo_wrapper.hpp; that keeps the
        # core lib free of CUDA dependencies on Free builds.
        if(EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/cuda_rckangaroo_backend.cpp")
            list(APPEND GPU_SOURCES src/gpu/cuda_rckangaroo_backend.cpp)
        endif()
    endif()

    # Check if source files exist
    set(EXISTING_GPU_SOURCES)
    foreach(SRC ${GPU_SOURCES})
        if(EXISTS "${CMAKE_SOURCE_DIR}/${SRC}")
            list(APPEND EXISTING_GPU_SOURCES ${SRC})
        endif()
    endforeach()

    if(EXISTING_GPU_SOURCES)
        add_library(starminer_gpu STATIC ${EXISTING_GPU_SOURCES})
        target_link_libraries(starminer_gpu
            ${CUDA_RT_TARGET}
        )
        # Ensure gpu/ is on the include path so hip_cuda_compat.hpp resolves
        # from both src/gpu/*.cu and src/gpu/v2/*.cu
        target_include_directories(starminer_gpu PRIVATE
            ${CMAKE_SOURCE_DIR}/src/gpu
        )
        # Link RCKangaroo if enabled
        if(STARMINER_USE_RCKANGAROO AND TARGET rckangaroo)
            target_link_libraries(starminer_gpu rckangaroo)
            target_include_directories(starminer_gpu PRIVATE
                ${CMAKE_SOURCE_DIR}/third_party/RCKangaroo
            )
        endif()
        set_target_properties(starminer_gpu PROPERTIES
            CUDA_SEPARABLE_COMPILATION ON
            CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            POSITION_INDEPENDENT_CODE ON
        )
    endif()

elseif(STARMINER_BACKEND STREQUAL "ROCM")
    # AMD ROCm/HIP: same .cu sources compiled with HIP language
    set(ROCM_GPU_SOURCES
        src/gpu/sha256.cu
        src/gpu/secp256k1.cu
        src/gpu/ripemd160.cu
        src/gpu/h160_bloom_filter.cu
        src/gpu/puzzle_gpu.cu
        src/gpu/puzzle_optimized.cu
        src/gpu/kangaroo_kernel.cu
    )
    set(EXISTING_ROCM_SOURCES)
    foreach(SRC ${ROCM_GPU_SOURCES})
        if(EXISTS "${CMAKE_SOURCE_DIR}/${SRC}")
            list(APPEND EXISTING_ROCM_SOURCES ${SRC})
        endif()
    endforeach()
    if(EXISTING_ROCM_SOURCES)
        # Tell CMake to compile .cu files as HIP (hipcc instead of nvcc)
        set_source_files_properties(${EXISTING_ROCM_SOURCES} PROPERTIES LANGUAGE HIP)
        add_library(starminer_gpu STATIC ${EXISTING_ROCM_SOURCES})
        target_link_libraries(starminer_gpu hip::host)
        set_target_properties(starminer_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

elseif(STARMINER_BACKEND STREQUAL "METAL")
    # Metal shaders are compiled separately
    # We use .metal files compiled to .metallib at build time
    set(METAL_SHADERS
        src/gpu/sha256.metal
        src/gpu/secp256k1.metal
        src/gpu/ripemd160.metal
        src/gpu/bloom_filter.metal
    )

    # Phase 11 (v1.4.0): Brain Wallet v2 puzzle-only Metal shader.
    # 1:1 port of the CUDA puzzle-mode kernel; loaded at runtime via the
    # CUDA-side gating.
    if(
       EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/v2/brain_wallet_v2.metal")
        list(APPEND METAL_SHADERS src/gpu/v2/brain_wallet_v2.metal)
    endif()

    # For now, create placeholder library
    # Metal compute kernels will be loaded at runtime
    add_library(starminer_gpu INTERFACE)

else()
    # CPU fallback - provide stub implementations
    add_library(starminer_gpu INTERFACE)
endif()

# ============================================================================
# Tools
# ============================================================================
if(STARMINER_BUILD_TOOLS)
    # UTXO Bloom Filter Builder
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/tools/build_bloom.cpp")
        add_executable(build_bloom
            src/tools/build_bloom.cpp
        )
        if(TARGET starminer_core)
            target_link_libraries(build_bloom
                starminer_core
                starminer_platform
                Threads::Threads
            )
        else()
            target_link_libraries(build_bloom
                starminer_platform
                Threads::Threads
            )
        endif()
    endif()

    # Banner Test Tool
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/tools/test_banner.cpp")
        add_executable(test_banner
            src/tools/test_banner.cpp
        )
        target_link_libraries(test_banner
            Threads::Threads
        )
    endif()
endif()

# ============================================================================
# Main Executable
# ============================================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.cpp")
    # For CUDA backend, include stub to enable device linking
    if(STARMINER_BACKEND STREQUAL "CUDA")
        add_executable(starminer src/main.cpp src/cuda_stub.cu)
    else()
        add_executable(starminer src/main.cpp)
    endif()

    # starminer_core PUBLIC-links starminer_platform (and the INTERFACE
    # fallback does the same), so starminer_platform flows through
    # transitively when we list starminer_core. Putting it ALSO in
    # MAIN_LIBS made Apple ld emit
    #    ld: warning: ignoring duplicate libraries: 'libstarminer_platform.a'
    # because the link line carried -lstarminer_platform twice. Drop the
    # explicit add; transitive resolution handles it.
    set(MAIN_LIBS Threads::Threads)

    if(TARGET starminer_core)
        list(APPEND MAIN_LIBS starminer_core)
    else()
        # Edge case: no CORE_SOURCES -> no starminer_core target was created.
        # Pull in starminer_platform directly so starminer still links.
        list(APPEND MAIN_LIBS starminer_platform)
    endif()

    if(TARGET starminer_gpu)
        list(APPEND MAIN_LIBS starminer_gpu)
    endif()

    if(TARGET starminer_pool)
        list(APPEND MAIN_LIBS starminer_pool)
    endif()

    if(TARGET starminer_license)
        list(APPEND MAIN_LIBS starminer_license)
    endif()

    if(STARMINER_BACKEND STREQUAL "CUDA")
        list(APPEND MAIN_LIBS ${CUDA_RT_TARGET})
    elseif(STARMINER_BACKEND STREQUAL "ROCM")
        list(APPEND MAIN_LIBS hip::host)
    elseif(STARMINER_BACKEND STREQUAL "METAL")
        list(APPEND MAIN_LIBS ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
    endif()

    target_link_libraries(starminer ${MAIN_LIBS})

    # For CUDA separable compilation, the final executable must resolve device symbols
    if(STARMINER_BACKEND STREQUAL "CUDA")
        set_target_properties(starminer PROPERTIES
            CUDA_SEPARABLE_COMPILATION ON
            CUDA_RESOLVE_DEVICE_SYMBOLS ON
            CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
        )
    endif()
endif()

# ============================================================================
# Tests
# ============================================================================
if(STARMINER_BUILD_TESTS)
    enable_testing()

    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_rule_engine.cpp" AND TARGET starminer_core)
        add_executable(test_rule_engine tests/test_rule_engine.cpp)
        target_link_libraries(test_rule_engine starminer_core)
        add_test(NAME RuleEngine COMMAND test_rule_engine)
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_priority_queue.cpp" AND TARGET starminer_core)
        add_executable(test_priority_queue tests/test_priority_queue.cpp)
        target_link_libraries(test_priority_queue starminer_core)
        add_test(NAME PriorityQueue COMMAND test_priority_queue)
    endif()

    # Platform tests
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_platform.cpp")
        add_executable(test_platform tests/test_platform.cpp)
        target_link_libraries(test_platform starminer_platform Threads::Threads)
        if(STARMINER_BACKEND STREQUAL "CUDA")
            target_link_libraries(test_platform ${CUDA_RT_TARGET})
        elseif(STARMINER_BACKEND STREQUAL "METAL")
            target_link_libraries(test_platform ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
        endif()
        add_test(NAME Platform COMMAND test_platform)
    endif()

    # Hash function verification tests
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_hash_vectors.cpp")
        add_executable(test_hash_vectors tests/test_hash_vectors.cpp)
        target_link_libraries(test_hash_vectors Threads::Threads)
        add_test(NAME HashVectors COMMAND test_hash_vectors)
    endif()

    # CLI parser + config-override matrix (Wave 5 / track-e, 2026-05-04).
    # Header-only test against src/core/yaml_config.hpp; mirrors the parse_args
    # in src/main.cpp (kept in sync; if you add a CLI flag, mirror it here).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_cli_parser.cpp")
        add_executable(test_cli_parser tests/test_cli_parser.cpp)
        target_link_libraries(test_cli_parser Threads::Threads)
        add_test(NAME CLIParser COMMAND test_cli_parser)
    endif()

    # KAT for range_bits_from_be in src/core/byte_codec.hpp. Pins the
    # canonical Bitcoin-puzzle ranges (75/135/160) to expected
    # range_bits values so a regression in the 256-bit subtraction +
    # bit_length helper would fail loudly. Used by Phase 1.4
    # CudaRCKangarooBackend::initialize() to size the kangaroo solver.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_range_bits.cpp")
        add_executable(test_range_bits tests/test_range_bits.cpp)
        # Header-only target -- byte_codec.hpp is inline-only.
        add_test(NAME RangeBitsKAT COMMAND test_range_bits)
    endif()

    # KAT for the rest of byte_codec.hpp: hex_encode_lower (~13 call
    # sites collapsed onto this in v1.4.0 phase 7.1) plus
    # be32_to_limbs_le / limbs_le_to_be32 roundtrip with the secp256k1
    # generator x-coordinate as the canonical test vector. Pure host
    # test, no GPU dependency.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_byte_codec.cpp")
        add_executable(test_byte_codec tests/test_byte_codec.cpp)
        add_test(NAME ByteCodecKAT COMMAND test_byte_codec)
    endif()

    # KAT for src/core/puzzle_config.hpp puzzle table: derive hash160
    # from each known solution_hex via cpu::compute_hash160 and verify
    # it matches the recorded target_h160_hex. Catches the data-
    # corruption class fixed in v1.4.0 phase 2.1 (mismatched addresses
    # + fabricated private keys). Pure host, runs everywhere.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_puzzle_table_kat.cpp")
        add_executable(test_puzzle_table_kat tests/test_puzzle_table_kat.cpp)
        add_test(NAME PuzzleTableKAT COMMAND test_puzzle_table_kat)
    endif()

    # Roundtrip KAT for KangarooWorkFile save/load (src/core/kangaroo.hpp).
    # The phase 1.8 mismatched-checkpoint rejection in
    # KangarooSolver::solve() depends on save/load being lossless;
    # this test pins down that contract.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_kangaroo_work_file.cpp")
        add_executable(test_kangaroo_work_file tests/test_kangaroo_work_file.cpp)
        add_test(NAME KangarooWorkFileKAT COMMAND test_kangaroo_work_file)
    endif()

    # Metal kangaroo throughput benchmark. Production-shaped config
    # (1024 kangaroos x 1024 steps x 32 rounds) so dispatch overhead
    # is amortized and the reported Mops/s reflects kernel throughput.
    # Used as the baseline for Phase 5 Montgomery batch inversion
    # before/after comparison. Mac-only.
    if(APPLE AND STARMINER_USE_METAL AND
       EXISTS "${CMAKE_SOURCE_DIR}/tests/bench_metal_kangaroo.mm")
        add_executable(bench_metal_kangaroo tests/bench_metal_kangaroo.mm)
        set_target_properties(bench_metal_kangaroo PROPERTIES
            COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(bench_metal_kangaroo
            starminer_core
            "-framework Foundation"
            "-framework Metal")
        # Not added as a ctest target (long-running, perf-only).
    endif()

    # Hashcat rule engine known-answer test. Pins the engine to externally
    # documented rule semantics so a regression vs. hashcat behaviour fails
    # loudly. Header-only, runs everywhere (no GPU / network required).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_rule_engine_kat.cpp" AND TARGET starminer_core)
        add_executable(test_rule_engine_kat tests/test_rule_engine_kat.cpp)
        target_link_libraries(test_rule_engine_kat starminer_core)
        add_test(NAME RuleEngineKAT COMMAND test_rule_engine_kat)
    endif()

    # Brain Wallet v2 host-side correctness test (Phase 9, v1.4.0).
    # The .cpp file has a CPU section that runs anywhere and a GPU section
    # gated on STARMINER_USE_CUDA. The kernel itself is wired into
    # half so puzzle-target mask math is regression-tested.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_brain_wallet_v2.cpp")
        add_executable(test_brain_wallet_v2 tests/v2/test_brain_wallet_v2.cpp)
        target_link_libraries(test_brain_wallet_v2 starminer_core)
        if(TARGET starminer_gpu)
            target_link_libraries(test_brain_wallet_v2 starminer_gpu)
        endif()
        target_include_directories(test_brain_wallet_v2 PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME BrainWalletV2 COMMAND test_brain_wallet_v2)
        # Skip exit code 77 if no CUDA device available at runtime.
        set_tests_properties(BrainWalletV2 PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Metal secp256k1 known-answer test (Mac only). Validates the field
    # arithmetic + point-add primitives in src/gpu/kangaroo.metal against
    # sipa/secp256k1 published priv-to-pub vectors. Returns 77 if no
    # Metal device is available so headless CI doesn't fail.
    if(APPLE AND STARMINER_USE_METAL
       AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_metal_secp256k1.mm")
        add_executable(test_metal_secp256k1 tests/test_metal_secp256k1.mm)
        set_source_files_properties(tests/test_metal_secp256k1.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(test_metal_secp256k1
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
        target_include_directories(test_metal_secp256k1
            PRIVATE ${KANGAROO_METAL_GENERATED_INCLUDE_DIR})
        add_test(NAME MetalSecp256k1KAT COMMAND test_metal_secp256k1)
        set_tests_properties(MetalSecp256k1KAT PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Metal Kangaroo smoke test: drives the kangaroo_step kernel through
    # the host dispatcher, asserts DP detection fires. Pairs with the
    # secp256k1 KAT to gate Mac Kangaroo shipping.
    if(APPLE AND STARMINER_USE_METAL
       AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_metal_kangaroo_smoke.mm"
       AND EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/kangaroo_metal.mm")
        add_executable(test_metal_kangaroo_smoke
            tests/test_metal_kangaroo_smoke.mm)
        set_source_files_properties(tests/test_metal_kangaroo_smoke.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        # The Metal dispatcher already lives in starminer_core (added to
        # CORE_SOURCES on Mac builds). Link the test against the lib so
        # we don't double-compile kangaroo_metal.mm and produce duplicate
        # symbols at link time.
        target_link_libraries(test_metal_kangaroo_smoke
            starminer_core
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
        target_include_directories(test_metal_kangaroo_smoke
            PRIVATE ${CMAKE_SOURCE_DIR}/src)
        add_test(NAME MetalKangarooSmoke COMMAND test_metal_kangaroo_smoke)
        set_tests_properties(MetalKangarooSmoke PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Metal fused puzzle pipeline KAT (v1.4.1). Runs privkey=1 / privkey=2
    # through the puzzle_search kernel and checks the recovered hash160
    # matches the published Bitcoin puzzle 1 / 2 vectors. A passing run
    # validates EC scalar mul, mod_inv, compress, SHA-256, RIPEMD-160,
    # and the compare-and-CAS logic end-to-end. Returns 77 if no Metal
    # device is present; ctest is configured with SKIP_RETURN_CODE=77.
    if(APPLE AND STARMINER_USE_METAL
       AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_metal_puzzle_kat.mm"
       AND EXISTS "${CMAKE_SOURCE_DIR}/src/gpu/puzzle_metal.mm")
        add_executable(test_metal_puzzle_kat
            tests/test_metal_puzzle_kat.mm)
        set_source_files_properties(tests/test_metal_puzzle_kat.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(test_metal_puzzle_kat
            starminer_core
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
        target_include_directories(test_metal_puzzle_kat
            PRIVATE ${CMAKE_SOURCE_DIR}/src)
        add_test(NAME MetalPuzzleKAT COMMAND test_metal_puzzle_kat)
        set_tests_properties(MetalPuzzleKAT PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Brain Wallet v2 host-side orchestrator test (Phase 9 follow-up).
    # Pure C++ test; we link starminer_gpu too because v2_orchestrator.cpp
    # (compiled into starminer_core) references kernels from starminer_gpu
    # under #if STARMINER_USE_CUDA -- the host code paths are dead in the
    # dry-run test, but the linker still needs the symbols.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_v2_orchestrator.cpp")
        add_executable(test_v2_orchestrator tests/v2/test_v2_orchestrator.cpp)
        target_link_libraries(test_v2_orchestrator starminer_core)
        if(TARGET starminer_gpu)
            target_link_libraries(test_v2_orchestrator starminer_gpu)
        endif()
        target_include_directories(test_v2_orchestrator PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME V2Orchestrator COMMAND test_v2_orchestrator)
    endif()

    # Phase 4: address-derivation CPU reference KATs.
    # Pure C++ -- header-only SHA-256 + RIPEMD-160 + hash160 reference,
    # plus the five address-type derivations. Validated against published
    # test vectors (BIP-49 / BIP-84 / BIP-86 / Bitcoin Core).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_address_derive_cpu.cpp")
        add_executable(test_address_derive_cpu tests/v2/test_address_derive_cpu.cpp)
        target_include_directories(test_address_derive_cpu PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME AddressDeriveCPU COMMAND test_address_derive_cpu)
    endif()

    # Phase 5: weak-PRNG CPU reference KATs.
    # MT19937, Park-Miller LCG, MSVC rand(), java.util.Random + the
    # libbitcoin bx and Profanity vulnerable constructions. Validated
    # against the official MT reference table, Java spec, and the known
    # MSVC rand() trace.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_weak_prng_cpu.cpp")
        add_executable(test_weak_prng_cpu tests/v2/test_weak_prng_cpu.cpp)
        target_include_directories(test_weak_prng_cpu PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME WeakPrngCPU COMMAND test_weak_prng_cpu)
    endif()

    # Phase 6: encoding-munger CPU reference KATs.
    # UTF-8 / UTF-16-LE / UTF-16-BE / UTF-32-LE / Latin-1 /
    # strip-non-ascii / upper-ascii / lower-ascii. Validated against
    # Python's stdlib codecs.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_encoding_munge_cpu.cpp")
        add_executable(test_encoding_munge_cpu tests/v2/test_encoding_munge_cpu.cpp)
        target_include_directories(test_encoding_munge_cpu PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME EncodingMungeCPU COMMAND test_encoding_munge_cpu)
    endif()

    # Phase 7/8: SHA-512 + HMAC + PBKDF2 reference KATs.
    # Validated against NIST CAVS, RFC 4231 (HMAC), RFC 6070 (PBKDF2),
    # and the BIP-39 reference vector for "abandon abandon ... about".
    # These are the building blocks for the legacy KDF framework
    # (Phase 7) and Electrum / BIP-39 mnemonic kernels (Phase 8).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_pbkdf2_cpu.cpp")
        add_executable(test_pbkdf2_cpu tests/v2/test_pbkdf2_cpu.cpp)
        target_include_directories(test_pbkdf2_cpu PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME Pbkdf2CPU COMMAND test_pbkdf2_cpu)
    endif()

    # Phase 8: Electrum v1 + v2 mnemonic CPU reference KATs.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/v2/test_electrum_cpu.cpp")
        add_executable(test_electrum_cpu tests/v2/test_electrum_cpu.cpp)
        target_include_directories(test_electrum_cpu PRIVATE
            ${CMAKE_SOURCE_DIR}
        )
        add_test(NAME ElectrumCPU COMMAND test_electrum_cpu)
    endif()

    # Phase 10: license cache lifecycle tests.
    # Validates the HMAC-bound cache that gates Pro features after a
    # successful remote validation. No filesystem / network -- the test
    # surface (make_cache_payload / compute_cache_hmac / verify_cache_entry)
    # is the algorithm under test.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_license_cache.cpp"
       AND TARGET starminer_license)
        add_executable(test_license_cache tests/test_license_cache.cpp)
        target_link_libraries(test_license_cache starminer_license)
        add_test(NAME LicenseCache COMMAND test_license_cache)
        set_tests_properties(LicenseCache PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # JLP pool TLS handshake live integration test. Runs a verified TLS
    # handshake, sends an AUTH frame, reads the response. Returns 77 (skip)
    # if STARMINER_SKIP_NETWORK_TESTS is set or TCP connect fails within 5
    # seconds. Cert/protocol failures are real FAILs.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_jlp_pool_handshake.cpp" AND OPENSSL_FOUND)
        add_executable(test_jlp_pool_handshake tests/test_jlp_pool_handshake.cpp)
        target_link_libraries(test_jlp_pool_handshake
            OpenSSL::SSL OpenSSL::Crypto Threads::Threads
        )
        if(WIN32)
            # ws2_32: WinSock for the test's own TCP/select code.
            # crypt32: CertOpenSystemStoreA / CertEnumCertificatesInStore /
            #          CertCloseStore. Required because vcpkg OpenSSL on
            #          Windows has no usable default trust store; the test
            #          bridges the Windows ROOT cert store into OpenSSL's
            #          X509_STORE so chain verification works.
            target_link_libraries(test_jlp_pool_handshake ws2_32 crypt32)
        endif()
        add_test(NAME JLPPoolHandshake COMMAND test_jlp_pool_handshake)
        set_tests_properties(JLPPoolHandshake PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # JLP pool URL parser + DistinguishedPoint serialization unit tests.
    # No sockets, no threads -- pure logic against the starminer_pool library.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_jlp_pool_manager.cpp" AND TARGET starminer_pool)
        add_executable(test_jlp_pool_manager tests/test_jlp_pool_manager.cpp)
        target_link_libraries(test_jlp_pool_manager starminer_pool Threads::Threads)
        add_test(NAME JLPPoolManager COMMAND test_jlp_pool_manager)
    endif()

    # JLP wire generated-vs-legacy drift detector. Static_asserts only,
    # plus three runtime byte-layout sanity checks. No threads, no sockets.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_jlp_wire_generated.cpp" AND TARGET starminer_pool)
        add_executable(test_jlp_wire_generated tests/test_jlp_wire_generated.cpp)
        target_link_libraries(test_jlp_wire_generated starminer_pool)
        if(TARGET jlp_codegen)
            add_dependencies(test_jlp_wire_generated jlp_codegen)
        endif()
        add_test(NAME JLPWireGenerated COMMAND test_jlp_wire_generated)
    endif()

    # JLP pool wire-protocol tests against an in-process mock TCP server on
    # 127.0.0.1 (use_tls=false). Drives the AUTH state machine, work
    # assignment, stats, DP submission, ping/pong, and pre-auth gating.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_jlp_pool_protocol.cpp" AND TARGET starminer_pool)
        add_executable(test_jlp_pool_protocol tests/test_jlp_pool_protocol.cpp)
        target_link_libraries(test_jlp_pool_protocol starminer_pool Threads::Threads)
        if(WIN32)
            # ws2_32 for the test's own mock-server WinSock calls (the
            # starminer_pool library transitively links it too, but listing it
            # here keeps the dependency explicit).
            target_link_libraries(test_jlp_pool_protocol ws2_32)
        endif()
        add_test(NAME JLPPoolProtocol COMMAND test_jlp_pool_protocol)
        # The protocol suite uses real threads + sockets; give it room.
        set_tests_properties(JLPPoolProtocol PROPERTIES TIMEOUT 60)
    endif()

    # JLP pool reconnect / disconnect handling tests. Mock TCP server, no TLS.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_jlp_pool_reconnect.cpp" AND TARGET starminer_pool)
        add_executable(test_jlp_pool_reconnect tests/test_jlp_pool_reconnect.cpp)
        target_link_libraries(test_jlp_pool_reconnect starminer_pool Threads::Threads)
        if(WIN32)
            target_link_libraries(test_jlp_pool_reconnect ws2_32)
        endif()
        add_test(NAME JLPPoolReconnect COMMAND test_jlp_pool_reconnect)
        set_tests_properties(JLPPoolReconnect PROPERTIES TIMEOUT 60)
    endif()

    # CUDA-only GPU correctness tests (Wave 0, 2026-05-04 review).
    # Each test is expected to FAIL on current code and PASS after Wave 1 crypto fixes.
    # Tests return CTest skip code (77) when no CUDA device is present at runtime.
    if(STARMINER_BACKEND STREQUAL "CUDA" AND TARGET starminer_gpu)
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_secp256k1_inv.cu")
            add_executable(test_secp256k1_inv tests/test_secp256k1_inv.cu)
            set_target_properties(test_secp256k1_inv PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_secp256k1_inv starminer_gpu ${CUDA_RT_TARGET})
            add_test(NAME Secp256k1Inv COMMAND test_secp256k1_inv)
            set_tests_properties(Secp256k1Inv PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        # Device HMAC-SHA512 KAT (RFC 4231 TC1/TC2/TC4/TC6). Pins the
        # output of device::hmac_sha512 in src/gpu/v2/device_hashes.cuh
        # against the published vectors -- catches regressions in the
        # streaming-rewrite stack-reduction refactor.
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_device_hmac_sha512.cu")
            add_executable(test_device_hmac_sha512 tests/test_device_hmac_sha512.cu)
            set_target_properties(test_device_hmac_sha512 PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_include_directories(test_device_hmac_sha512
                PRIVATE ${CMAKE_SOURCE_DIR}/src)
            target_link_libraries(test_device_hmac_sha512 ${CUDA_RT_TARGET})
            add_test(NAME DeviceHmacSha512KAT COMMAND test_device_hmac_sha512)
            set_tests_properties(DeviceHmacSha512KAT PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_ec_table_consistency.cu")
            add_executable(test_ec_table_consistency tests/test_ec_table_consistency.cu)
            set_target_properties(test_ec_table_consistency PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_ec_table_consistency starminer_gpu ${CUDA_RT_TARGET})
            add_test(NAME EcTableConsistency COMMAND test_ec_table_consistency)
            set_tests_properties(EcTableConsistency PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        # Wave 1 Track C C-CRIT-2 follow-up (2026-05-04). Validates the
        # binary-exponentiation mod_inv in puzzle_optimized.cu (separate copy
        # from secp256k1.cu's mod_inv because U256 here is 4 x uint64).
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_puzzle_optimized_inv.cu")
            add_executable(test_puzzle_optimized_inv tests/test_puzzle_optimized_inv.cu)
            set_target_properties(test_puzzle_optimized_inv PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_puzzle_optimized_inv starminer_gpu ${CUDA_RT_TARGET})
            add_test(NAME PuzzleOptimizedInv COMMAND test_puzzle_optimized_inv)
            set_tests_properties(PuzzleOptimizedInv PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        # GpuHash160 exercises the fused brain-wallet pipeline
        # (fused_pipeline_init + fused_brain_wallet_batch_fixed_stride) and is
        # therefore a Pro-only test. In the free build the brain-wallet
        # kernels and host wrappers do not exist in the binary, so the test
        # cannot link.
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_gpu_hash160.cu" AND TARGET starminer_core)
            add_executable(test_gpu_hash160 tests/test_gpu_hash160.cu)
            set_target_properties(test_gpu_hash160 PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_gpu_hash160 starminer_gpu starminer_core ${CUDA_RT_TARGET})
            add_test(NAME GpuHash160 COMMAND test_gpu_hash160)
            set_tests_properties(GpuHash160 PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        # EC scalar multiplication known-answer test. Hands the GPU a few well
        # known privkeys (1, 2, 3, 7), reads back the affine pubkey, encodes
        # it as the standard 33-byte compressed pubkey, and compares against
        # the canonical secp256k1 generator multiples. Catches regressions in
        # the entire EC pipeline (mod_mul, mod_inv, ec_double, ec_add_mixed,
        # jacobian_to_affine, precomputed-table generation).
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_ec_mul_known_answers.cu")
            add_executable(test_ec_mul_known_answers tests/test_ec_mul_known_answers.cu)
            set_target_properties(test_ec_mul_known_answers PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_ec_mul_known_answers starminer_gpu ${CUDA_RT_TARGET})
            add_test(NAME EcMulKnownAnswers COMMAND test_ec_mul_known_answers)
            set_tests_properties(EcMulKnownAnswers PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        # Known-answer test for the puzzle / kangaroo EC multiplication path
        # (puzzle_optimized.cu's ec_mul_glv). Calls a dedicated test kernel
        # that runs ec_mul_glv on a handful of small scalars and writes the
        # affine pubkey back to host; the host re-encodes as compressed
        # pubkey and byte-compares against known answers (same vectors as
        # EcMulKnownAnswers, but exercising the puzzle_optimized.cu code path
        # rather than secp256k1.cu). Hash chain coverage lives in GpuHash160
        # and test_hash_vectors; splitting the concerns gives actionable
        # failure signals.
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_kangaroo_small_puzzle.cu")
            add_executable(test_kangaroo_small_puzzle tests/test_kangaroo_small_puzzle.cu)
            set_target_properties(test_kangaroo_small_puzzle PROPERTIES
                CUDA_SEPARABLE_COMPILATION ON
                CUDA_RUNTIME_LIBRARY $<IF:$<BOOL:${WIN32}>,Static,Shared>
            )
            target_link_libraries(test_kangaroo_small_puzzle starminer_gpu ${CUDA_RT_TARGET})
            add_test(NAME KangarooSmallPuzzle COMMAND test_kangaroo_small_puzzle)
            set_tests_properties(KangarooSmallPuzzle PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()

# ============================================================================
# Benchmarks
# ============================================================================
if(STARMINER_BUILD_BENCHMARKS AND STARMINER_BACKEND STREQUAL "CUDA")
    if(EXISTS "${CMAKE_SOURCE_DIR}/benchmarks/bench_gpu_pipeline.cu")
        add_executable(bench_gpu_pipeline benchmarks/bench_gpu_pipeline.cu)
        target_link_libraries(bench_gpu_pipeline
            starminer_gpu
            ${CUDA_RT_TARGET}
        )
    endif()

    # v1.4.1 BW-DEDUP-3: bench_bloom_ab.cu was used to pick between
    # the two bloom impls. After the verdict (B-texture wins), the
    # losing impl was deleted and the bench retired -- its purpose
    # is served. A future B-global-vs-B-texture bench would be a
    # different harness (h160_bloom_filter.cu has no CUDA insert
    # wrapper, so the throughput-only comparison would look quite
    # different).
endif()

# ============================================================================
# Installation
# ============================================================================
if(TARGET starminer)
    install(TARGETS starminer RUNTIME DESTINATION bin)
endif()

if(TARGET build_bloom)
    install(TARGETS build_bloom RUNTIME DESTINATION bin)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/rules")
    install(DIRECTORY rules/ DESTINATION share/starminer/rules)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/wordlists")
    install(DIRECTORY wordlists/ DESTINATION share/starminer/wordlists)
endif()

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "StarMiner Configuration Summary:")
message(STATUS "  Platform:    ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Backend:     ${STARMINER_BACKEND}")
message(STATUS "  Build Type:  ${CMAKE_BUILD_TYPE}")
if(STARMINER_BACKEND STREQUAL "CUDA")
    message(STATUS "  CUDA Archs:  ${CMAKE_CUDA_ARCHITECTURES}")
endif()
message(STATUS "  Tests:       ${STARMINER_BUILD_TESTS}")
message(STATUS "  Benchmarks:  ${STARMINER_BUILD_BENCHMARKS}")
message(STATUS "  Tools:       ${STARMINER_BUILD_TOOLS}")
message(STATUS "")
