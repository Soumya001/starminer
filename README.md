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
# Linux / macOS
chmod +x starminer-linux-x64-cuda
./starminer-linux-x64-cuda --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS

# Windows
.\starminer-windows-x64-cuda.exe --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS
```

Or launch without arguments for the interactive menu:

```bash
./starminer-linux-x64-cuda
```

## Platform binaries

| Platform | Binary | GPU backend |
|----------|--------|-------------|
| Linux x64 | `starminer-linux-x64-cuda` | NVIDIA CUDA |
| Linux x64 | `starminer-linux-x64-rocm` | AMD ROCm/HIP |
| Linux x64 | `starminer-linux-x64-cpu` | CPU (any) |
| Windows x64 | `starminer-windows-x64-cuda.exe` | NVIDIA CUDA |
| Windows x64 | `starminer-windows-x64-cpu.exe` | CPU (any) |
| macOS ARM64 | `starminer-macos-arm64` | Apple Metal |

## Features

- **JLP binary pool protocol** — TLS-encrypted, low-overhead wire format (`jlps://`)
- **One-line installer** — auto-detects OS and GPU, no dependencies required
- **Interactive menu** — launch without flags; the UI guides configuration
- **Multi-GPU** — `--gpus 0,1,2,3`
- **Auto-reconnect** — jittered exponential backoff, survives network drops
- **Auto-update check** — notifies when a new release is available
- **Persistent config** — first run saves `~/.starminer/config.yml`; subsequent runs need no flags

## Pool

The public pool is at **starnetlive.space**. Dashboard: <https://starnetlive.space/puzzle135>

```bash
./starminer --pool jlps://starnetlive.space:17403 --worker YOUR_BTC_ADDRESS
```

`YOUR_BTC_ADDRESS` is your worker identity and payout address. Use any valid Bitcoin address you control. Your share of the pool is proportional to your contributed Distinguished Points. See [docs/POOL.md](docs/POOL.md) for full details.

## Build from source

```bash
git clone https://github.com/Soumya001/starminer.git
cd starminer

# CUDA (Linux / Windows)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_CUDA=ON
cmake --build build --target starminer -j$(nproc)

# CPU only
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARMINER_USE_CUDA=OFF
cmake --build build --target starminer -j$(nproc)
```

Output: `build/starminer` (Linux/macOS) or `build\starminer.exe` (Windows).

For full platform-specific build instructions see [docs/INSTALL.md](docs/INSTALL.md).

## CLI reference

```
--pool,   -p <url>     Pool URL   (e.g. jlps://starnetlive.space:17403)
--worker, -w <addr>    Worker name / BTC address for rewards
--gpus,   -g <ids>     GPU device IDs, comma-separated (default: all)
--no-update-check      Skip the background version check at startup
--verbose              Extra logging (DP submissions, reconnects)
--debug                Dump resolved config at startup
--benchmark            Run a timed GPU benchmark and exit
--help                 Show full help
```

## File structure

```
starminer/
├── src/
│   ├── cli/          # CLI parser
│   ├── core/         # Config, YAML, update checker
│   ├── gpu/          # CUDA / Metal / CPU kernels
│   ├── pool/         # JLP pool client (TLS)
│   └── runtime/      # Mode dispatch (pool, puzzle, benchmark)
├── docs/             # INSTALL, POOL, CONFIGURATION, CHANGELOG …
├── pool/installer/   # Standalone install.sh / install.ps1 / install.py
├── third_party/      # xxHash, nlohmann/json
└── CMakeLists.txt
```

## License

GPLv3. See `LICENSE`.
