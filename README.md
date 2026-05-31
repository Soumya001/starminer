<div align="center">

# ₿ StarMiner

**GPU-accelerated Pollard's Kangaroo solver for Bitcoin Puzzle #135**

~13.5 BTC prize · Collaborative pool · NVIDIA / AMD / Apple Silicon / CPU

[![Latest Release](https://img.shields.io/github/v/release/Soumya001/starminer?color=22d3ee&label=release)](https://github.com/Soumya001/starminer/releases/latest)
[![Dashboard](https://img.shields.io/badge/dashboard-starnetlive.space-22d3ee)](https://starnetlive.space)
[![License](https://img.shields.io/badge/license-GPLv3-34d399)](LICENSE)

</div>

---

## Download

> **Easiest:** use the one-line installers below — auto-detects GPU, downloads binary, starts mining.
> **Manual:** grab the binary directly for your platform.

| Platform | Binary | GPU |
|---|---|---|
| Linux x64 | [starminer-linux-x64-cuda](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-cuda) | NVIDIA CUDA |
| Linux x64 | [starminer-linux-x64-rocm](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-rocm) | AMD ROCm / HIP |
| Linux x64 | [starminer-linux-x64-cpu](https://github.com/Soumya001/starminer/releases/latest/download/starminer-linux-x64-cpu) | CPU only |
| Windows x64 | [starminer-windows-x64-cuda.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-cuda.exe) | NVIDIA CUDA |
| Windows x64 | [starminer-windows-x64-hip.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-hip.exe) | AMD RX 5000/6000/7000 |
| Windows x64 | [starminer-windows-x64-cpu.exe](https://github.com/Soumya001/starminer/releases/latest/download/starminer-windows-x64-cpu.exe) | CPU only |
| macOS ARM64 | [starminer-macos-arm64](https://github.com/Soumya001/starminer/releases/latest/download/starminer-macos-arm64) | Apple Silicon |

---

## Install

### 🪟 Windows
```powershell
irm https://starnetlive.space/install-135.ps1 | iex
```
Auto-detects GPU, downloads binary, creates a desktop shortcut. No Python or build tools required.

---

### 🐧 Linux / macOS
```bash
curl -fsSL https://starnetlive.space/install-135.sh | bash
```

---

### ☁️ Vast.ai / Headless / Datacenter
```bash
# Headless — replace with your BTC address, no prompts
WORKER_NAME=YOUR_BTC_ADDRESS curl -fsSL https://starnetlive.space/install-135.sh | bash
```

**Vast.ai on-start script** — paste into your instance template:
```bash
WORKER_NAME=YOUR_BTC_ADDRESS curl -fsSL https://starnetlive.space/install-135.sh | bash
```

The installer auto-installs the CUDA runtime if missing (`libcudart.so.12`), assigns your device a persistent ID, and starts mining immediately.

---

### ⚡ Manual (after downloading binary)

```bash
# Linux / macOS
chmod +x starminer-linux-x64-cuda
./starminer-linux-x64-cuda --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Windows
.\starminer-windows-x64-cuda.exe --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS

# Multiple GPUs
./starminer-linux-x64-cuda --pool jlp://pool.starnetlive.space:5678 --worker YOUR_BTC_ADDRESS --gpus 0,1,2
```

---

## Pool

| | |
|---|---|
| **Pool URL** | `jlp://pool.starnetlive.space:5678` |
| **Dashboard** | [starnetlive.space](https://starnetlive.space) |
| **Protocol** | JLP — binary TCP, direct connection |
| **Puzzle** | #135 · key range [2¹³⁴, 2¹³⁵−1] |
| **Algorithm** | Pollard's Kangaroo · O(√N) ≈ 2⁶⁷ steps |
| **DP Bits** | 28 · each DP ≈ 268M kangaroo steps |
| **Prize** | ~13.5 BTC · `1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH` |
| **Payouts** | Proportional to DP share · manual BTC on solve |

---

## Multi-rig Setup

Each device gets a **stable unique ID** on first install, saved in `worker.json`. Multiple machines using the same wallet are tracked separately and combined for payouts automatically.

```
Machine A  →  bc1q.../a3f7c9e12b04   ← auto-generated, persists across reinstalls
Machine B  →  bc1q.../f8b2d3a19c71   ← separate device, separate tracking

Dashboard  →  shows both rigs individually
Payout     →  bc1q... gets (rig A DPs + rig B DPs) combined
```

Reinstalling preserves your device ID and contribution history.

---

## Features

| Feature | Description |
|---|---|
| **Multi-platform GPU** | NVIDIA CUDA · AMD ROCm/HIP · Apple Metal · CPU fallback |
| **JLP pool protocol** | Binary TCP wire format · auto-reconnect with backoff |
| **Device identity** | Stable UUID per machine · survives reinstalls |
| **Multi-GPU** | `--gpus 0,1,2` or `--gpus all` |
| **CUDA auto-install** | Detects and fixes missing `libcudart.so.12` on Linux |
| **Stale chunk detection** | Server pushes fresh work if range is exhausted |
| **Thermal monitoring** | NVML integration · GPU temperature logging |
| **Brain wallet** | SHA-256 → secp256k1 → RIPEMD-160 + bloom filter |
| **Range scan** | Sweep raw key ranges against bloom filter |

---

## Build from Source

```bash
git clone https://github.com/Soumya001/starminer.git
cd starminer

# NVIDIA CUDA — Linux / Windows
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_CUDA=ON
cmake --build build --target starminer -j$(nproc)

# AMD ROCm/HIP — Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_ROCM=ON
cmake --build build --target starminer -j$(nproc)

# CPU only
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSTARMINER_USE_CUDA=OFF -DSTARMINER_USE_ROCM=OFF
cmake --build build --target starminer -j$(nproc)
```

Output: `build/starminer` (Linux/macOS) · `build\starminer.exe` (Windows)

---

## CLI Reference

```
--pool,   -p <url>     Pool URL           jlp://pool.starnetlive.space:5678
--worker, -w <addr>    BTC address        worker identity and payout address
--gpus,   -g <ids>     GPU IDs            comma-separated or "all" (default: all)
--puzzle, -P [N]       Puzzle number      default: auto-detect
--benchmark            GPU benchmark and exit
--brainwallet          Brain wallet scan  requires --wordlist + --bloom
--wordlist <file>      Passphrase list    for brain wallet mode
--bloom <file.blf>     Bloom filter       funded addresses
--range-scan           Key range sweep    requires --bloom
--no-update-check      Skip version check at startup
--verbose              Extra logging
--help                 Full help text
```

---

## Support / Donate

If StarMiner or the pool has been useful to you, donations are appreciated:

<table><tr>
<td valign="middle"><b>Bitcoin</b><br><code>bc1qevyu9pngzdq54v592whjf9tm5mcztv46zpu40p</code></td>
<td><img src="https://api.qrserver.com/v1/create-qr-code/?size=120x120&data=bc1qevyu9pngzdq54v592whjf9tm5mcztv46zpu40p" alt="BTC QR"/></td>
</tr></table>

---

## License

GPLv3 — see [`LICENSE`](LICENSE)
