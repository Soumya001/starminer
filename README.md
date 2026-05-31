# StarMiner

**StarMiner** is a GPU-accelerated Bitcoin puzzle solver with native JLP pool support. It targets [Puzzle #135](https://starnetlive.space) using Pollard's Kangaroo algorithm and connects to the **starnetlive.space** collaborative mining pool.

## One-line install (recommended)

**Linux / macOS:**
```bash
curl -fsSL https://starnetlive.space/install-135.sh | bash
```

**Windows (PowerShell):**
```powershell
irm https://starnetlive.space/install-135.ps1 | iex
```

The installer auto-detects your GPU, downloads the right binary, assigns your device a persistent ID (so reinstalls keep your history), and creates a desktop shortcut. No build tools required.

## Vast.ai / Headless / Datacenter

If the one-liner is blocked by a firewall or CDN, use the GitHub raw URL directly:

```bash
# Interactive
python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)

# Headless — set WORKER_NAME to your BTC address, no prompts
WORKER_NAME=YOUR_BTC_ADDRESS python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)
```

**Vast.ai on-start script** (paste into instance template):
```bash
WORKER_NAME=YOUR_BTC_ADDRESS python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)
```

## Multi-rig / multiple GPUs

Each device gets a **stable unique ID** automatically. Multiple machines using the same wallet are tracked separately in the pool and combined for payouts:

```
Machine 1 → wallet/a3f7c9e12b04   (auto-assigned, saved in worker.json)
Machine 2 → wallet/f8b2d3a19c71   (different machine, different ID)
Payouts   → combined for your wallet
```

The device ID is preserved across reinstalls — your contribution history is never reset.

## Manual quick start

Download from [Releases](https://github.com/Soumya001/starminer/releases/latest):

```bash
# Linux / macOS — NVIDIA
./starminer-linux-x64-cuda --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Linux — AMD
./starminer-linux-x64-rocm --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Windows — NVIDIA
.\starminer-windows-x64-cuda.exe --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Windows — AMD
.\starminer-windows-x64-hip.exe --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS
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

## Pool

- **Pool URL:** `jlp://pool.starnetlive.space:5678`
- **Dashboard:** <https://starnetlive.space>
- **Protocol:** JLP (plaintext TCP) — worker connects directly, no HTTP overhead
- **DP bits:** 28 — each DP represents ~268M kangaroo steps
- **Payouts:** proportional to DP contribution at time of solve, manual BTC transfer

```bash
./starminer --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS
```

## Features

- **JLP pool protocol** — binary wire format, auto-reconnect with backoff
- **Multi-platform GPU** — NVIDIA CUDA, AMD ROCm/HIP, Apple Metal, CPU fallback
- **Persistent device identity** — stable UUID per machine, survives reinstalls
- **Multi-GPU** — `--gpus 0,1,2,3`
- **Stale chunk detection** — server pushes fresh work if chunk is exhausted
- **Thermal monitoring** — NVML integration, logs GPU temperature
- **Brain wallet scanner** — SHA-256 → secp256k1 → RIPEMD-160 with bloom filter
- **Range scan** — sweep key ranges against bloom filter

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

# CPU only
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSTARMINER_USE_CUDA=OFF -DSTARMINER_USE_ROCM=OFF
cmake --build build --target starminer -j$(nproc)
```

## CLI reference

```
--pool,   -p <url>     Pool URL   (e.g. jlp://pool.starnetlive.space:5678)
--worker, -w <addr>    Worker name / BTC address for rewards
--gpus,   -g <ids>     GPU device IDs, comma-separated (default: all)
--puzzle, -P [N]       Target puzzle number (default: auto-select)
--benchmark            Run a timed GPU benchmark and exit
--brainwallet          Brain wallet scanning mode (requires --wordlist + --bloom)
--wordlist <file>      Passphrase list for brain wallet mode
--bloom <file.blf>     Bloom filter of funded addresses
--range-scan           Sweep key ranges against bloom filter
--no-update-check      Skip background version check at startup
--verbose              Extra logging
--help                 Show full help
```

## License

GPLv3. See `LICENSE`.
