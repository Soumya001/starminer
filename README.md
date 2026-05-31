# StarMiner

**StarMiner** is a GPU-accelerated Bitcoin puzzle solver targeting [Puzzle #135](https://starnetlive.space) — ~13.5 BTC prize — using Pollard's Kangaroo algorithm on the **StarNet collaborative pool**.

🌐 **Dashboard:** [starnetlive.space](https://starnetlive.space) &nbsp;|&nbsp; 📦 **Releases:** [Latest](https://github.com/Soumya001/starminer/releases/latest)

---

## Download

### Direct links (latest release)

| Platform | Download | GPU |
|---|---|---|
| Linux x64 | [starminer-linux-x64-cuda](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-cuda) | NVIDIA CUDA |
| Linux x64 | [starminer-linux-x64-rocm](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-rocm) | AMD ROCm/HIP |
| Linux x64 | [starminer-linux-x64-cpu](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-cpu) | CPU only |
| Windows x64 | [starminer-windows-x64-cuda.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-cuda.exe) | NVIDIA CUDA |
| Windows x64 | [starminer-windows-x64-hip.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-hip.exe) | AMD RX 5000/6000/7000 |
| Windows x64 | [starminer-windows-x64-cpu.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-cpu.exe) | CPU only |
| macOS ARM64 | [starminer-macos-arm64](https://github.com/Soumya001/starminer/releases/latest/download/starminer-macos-arm64) | Apple Silicon |

---

## Quick Install

### Windows
```powershell
irm https://starnetlive.space/install-135.ps1 | iex
```
Auto-detects GPU, downloads binary, creates desktop shortcut. No Python needed.

### Linux / macOS
```bash
curl -fsSL https://starnetlive.space/install-135.sh | bash
```

### Vast.ai / Headless / Datacenter

Datacenter IPs may be blocked by CDN on the above URLs. Use GitHub directly — **no CDN, always works:**

```bash
# Interactive
python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)

# Headless — no prompts, WORKER_NAME sets your BTC address
WORKER_NAME=YOUR_BTC_ADDRESS python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)
```

**Vast.ai template → On-start script** (replace address):
```bash
WORKER_NAME=YOUR_BTC_ADDRESS python3 <(curl -fsSL https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py)
```

---

## Manual start (after download)

```bash
# Linux / macOS — make executable first
chmod +x starminer-linux-x64-cuda

# NVIDIA
./starminer-linux-x64-cuda --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# AMD
./starminer-linux-x64-rocm --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Windows (NVIDIA)
.\starminer-windows-x64-cuda.exe --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Multi-GPU
./starminer-linux-x64-cuda --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS --gpus 0,1,2
```

---

## Multi-rig / Multiple machines

Each device gets a **stable unique ID** generated at install time and saved to `worker.json`. Multiple machines with the same wallet are tracked separately in the pool — payouts are combined by wallet at solve time.

```
Machine 1 → bc1q.../a3f7c9e12b04   (auto ID, persists across reinstalls)
Machine 2 → bc1q.../f8b2d3a19c71   (different machine, different ID)
Dashboard → shows each rig separately
Payout    → bc1q... total = rig1 + rig2 DPs combined
```

The device ID **survives reinstalls** — reinstalling keeps your contribution history.

---

## Pool

| | |
|---|---|
| **Pool URL** | `jlp://pool.starnetlive.space:5678` |
| **Protocol** | JLP — binary TCP, direct connection, no HTTP overhead |
| **Dashboard** | [starnetlive.space](https://starnetlive.space) |
| **Puzzle** | #135 — key range 2^134 to 2^135−1 |
| **Algorithm** | Pollard's Kangaroo — O(√N) ≈ 2^67 steps |
| **DP bits** | 28 — each DP ≈ 268M kangaroo steps |
| **Prize** | ~13.5 BTC at `1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH` |
| **Payouts** | Proportional to DP share, manual BTC transfer on solve |

---

## Features

- **JLP pool protocol** — low-overhead binary wire format, auto-reconnect
- **Multi-platform** — NVIDIA CUDA, AMD ROCm/HIP, Apple Metal, CPU
- **Persistent device identity** — stable UUID per machine, survives reinstalls
- **Multi-GPU** — `--gpus 0,1,2,3` or `--gpus all`
- **Stale chunk detection** — server auto-reassigns fresh work if chunk exhausted
- **Thermal monitoring** — NVML integration, GPU temperature logging
- **Brain wallet scanner** — SHA-256 → secp256k1 → RIPEMD-160 + bloom filter
- **Range scan** — sweep raw key ranges against a bloom filter

---

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

---

## CLI reference

```
--pool,   -p <url>     Pool URL  (jlp://pool.starnetlive.space:5678)
--worker, -w <addr>    BTC address — worker identity and payout address
--gpus,   -g <ids>     GPU IDs, comma-separated or "all" (default: all)
--puzzle, -P [N]       Target puzzle number (default: auto)
--benchmark            GPU benchmark and exit
--brainwallet          Brain wallet scan mode (requires --wordlist + --bloom)
--wordlist <file>      Passphrase list for brain wallet mode
--bloom <file.blf>     Bloom filter of funded addresses
--range-scan           Sweep key ranges against bloom filter
--no-update-check      Skip version check at startup
--verbose              Extra logging
--help                 Full help
```

---

## License

GPLv3. See `LICENSE`.
