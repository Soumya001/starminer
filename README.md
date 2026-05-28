# StarMiner

**StarMiner** is a GPU-accelerated Bitcoin puzzle solver with native JLP pool support. It targets [Puzzle #135](https://starnetlive.space/puzzle135) using Pollard's Kangaroo algorithm and connects to the **starnetlive.space** collaborative mining pool.

## One-line install (recommended)

**Linux / macOS:**

```bash
curl -fsSL https://starnetlive.space/install-135.sh | bash
```

**Windows (PowerShell):**

```powershell
irm https://starnetlive.space/install-135.ps1 | iex
```

The installer auto-detects your GPU, downloads the right binary from the latest GitHub release, creates a desktop shortcut, and optionally starts mining immediately. No Python or build tools required.

## Manual quick start

Download the binary for your platform from [Releases](https://github.com/Soumya001/starminer/releases/latest), then:

```bash
# Linux / macOS — NVIDIA
./starminer-linux-x64-cuda --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS

# Linux — AMD
./starminer-linux-x64-rocm --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS

# Windows — NVIDIA
.\starminer-windows-x64-cuda.exe --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS

# Windows — AMD (RX 5000/6000/7000)
.\starminer-windows-x64-hip.exe --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS
```

Or launch without arguments for the interactive menu:

```bash
./starminer-linux-x64-cuda
```

## Platform binaries

| Platform | Binary | GPU |
|----------|--------|-----|
| Linux x64 | `starminer-linux-x64-cuda` | NVIDIA (CUDA) |
| Linux x64 | `starminer-linux-x64-rocm` | AMD (ROCm/HIP) |
| Linux x64 | `starminer-linux-x64-cpu` | CPU only |
| Windows x64 | `starminer-windows-x64-cuda.exe` | NVIDIA (CUDA) |
| Windows x64 | `starminer-windows-x64-hip.exe` | AMD RX 5000/6000/7000 (HIP) |
| Windows x64 | `starminer-windows-x64-cpu.exe` | CPU only |
| macOS ARM64 | `starminer-macos-arm64` | Apple Silicon (Metal) |

## Features

- **JLP pool protocol** — TLS-encrypted, low-overhead binary wire format (`jlps://`)
- **Multi-platform GPU** — NVIDIA CUDA, AMD ROCm/HIP, Apple Metal, CPU fallback
- **One-line installer** — auto-detects OS and GPU, no dependencies required
- **Interactive menu** — launch without flags; guided setup for pool, puzzle, or brain wallet mode
- **Multi-GPU** — `--gpus 0,1,2,3`
- **Auto-reconnect** — jittered exponential backoff, survives network drops
- **Auto-update check** — notifies when a new release is available
- **Persistent config** — saves to `~/.starminer/config.yml` on first run
- **Brain wallet scanner** — hash passphrases (SHA-256 → secp256k1 → RIPEMD-160) and check against a bloom filter of funded addresses (`--brainwallet`)
- **Range scan** — sweep raw key ranges against a bloom filter (`--range-scan`)

## Pool

The public pool is at **starnetlive.space**. Dashboard: <https://starnetlive.space/puzzle135>

```bash
./starminer --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS
```

`YOUR_BTC_ADDRESS` is your worker identity and payout address. Use any valid Bitcoin address you control. Your share is proportional to contributed Distinguished Points. See [docs/POOL.md](docs/POOL.md) for details.

## Build from source

```bash
git clone https://github.com/Soumya001/starminer.git
cd starminer

# NVIDIA CUDA (Linux / Windows)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_CUDA=ON
cmake --build build --target starminer -j$(nproc)

# AMD ROCm/HIP (Linux)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_ROCM=ON
cmake --build build --target starminer -j$(nproc)

# AMD HIP (Windows) — requires AMD HIP SDK at C:\AMD\HIP
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_ROCM=ON ^
      -DCMAKE_CXX_COMPILER="C:/AMD/HIP/bin/clang++.exe"
cmake --build build --target starminer -j4

# CPU only
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DSTARMINER_USE_CUDA=OFF -DSTARMINER_USE_ROCM=OFF
cmake --build build --target starminer -j$(nproc)
```

Output: `build/starminer` (Linux/macOS) or `build\starminer.exe` (Windows).

For full platform-specific instructions see [docs/INSTALL.md](docs/INSTALL.md).

## CLI reference

```
--pool,   -p <url>     Pool URL   (e.g. jlps://starnetlive.space:17403)
--worker, -w <addr>    Worker name / BTC address for rewards
--gpus,   -g <ids>     GPU device IDs, comma-separated (default: all)
--puzzle, -P [N]       Target puzzle number (default: auto-select)
--benchmark            Run a timed GPU benchmark and exit
--brainwallet          Brain wallet scanning mode (requires --wordlist + --bloom)
--wordlist <file>      Passphrase list for brain wallet mode
--bloom <file.blf>     Bloom filter of funded addresses
--range-scan           Sweep key ranges against bloom filter (requires --bloom)
--min-bits <N>         Start bit width for range scan (default: 1)
--max-bits <N>         End bit width for range scan (default: 50)
--no-update-check      Skip the background version check at startup
--verbose              Extra logging
--debug                Dump resolved config at startup
--help                 Show full help
```

## File structure

```
starminer/
├── src/
│   ├── cli/          # CLI parser
│   ├── core/         # Config, YAML, update checker
│   ├── gpu/          # CUDA / HIP / Metal kernels + compat layer
│   ├── pool/         # JLP pool client (TLS)
│   ├── runtime/      # Mode runners (pool, puzzle, brain wallet, benchmark)
│   └── tools/        # build_bloom — build .blf bloom filters from UTXO dumps
├── docs/             # INSTALL, POOL, CONFIGURATION, CHANGELOG
├── third_party/      # xxHash, nlohmann/json
└── CMakeLists.txt
```

## License

GPLv3. See `LICENSE`.
