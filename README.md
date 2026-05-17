# StarMiner

**StarMiner** is a GPU-accelerated Bitcoin puzzle solver with native HTTP/JSON pool support. Forked and heavily improved from StarMiner — no JLP binary protocol required.

## Quick Start

### Linux / macOS (with Python)

```bash
wget https://starnetlive.space/download/starminer-worker.py
python3 starminer-worker.py --pool https://starnetlive.space --worker YOUR_BTC_ADDRESS --gpus 0
```

### Windows (with Python)

```batch
curl -o starminer-worker.py https://starnetlive.space/download/starminer-worker.py
python starminer-worker.py --pool https://starnetlive.space --worker YOUR_BTC_ADDRESS --gpus 0
```

### Windows (double-click .exe — no Python needed)

Download `starminer-worker.exe` from [Releases](../../releases) and double-click.

## Features

- **HTTP/JSON Pool** — `--pool https://yourserver.com` (no JLP required)
- **Auto-Download** — Detects your OS/GPU and downloads the right binary
- **Live Dashboard** — Terminal UI showing hash rate, DPs, pool share
- **Multi-GPU** — `--gpus 0,1,2,3`
- **Auto-Restart** — Crashes? It reconnects automatically
- **Heartbeat** — Keeps you marked active on the pool

## Build from Source

```bash
mkdir build && cd build
cmake .. -DCOLLIDER_USE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86
make starminer -j$(nproc)
```

## Pool API

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/pool/auth` | POST | Worker auth |
| `/api/v1/pool/work` | POST | Get work chunk |
| `/api/v1/pool/dp_batch` | POST | Submit DPs |
| `/api/v1/pool/solution` | POST | Report key found |
| `/api/v1/pool/stats` | GET | Pool stats |
| `/api/v1/pool/ping` | POST | Heartbeat |

## File Structure

```
starminer/
├── src/              # C++ solver code
│   ├── pool/         # HttpPoolClient (NEW)
│   ├── gpu/          # CUDA / Metal kernels
│   └── ...
├── worker/           # Universal worker scripts
│   ├── starminer-worker.py   # One-file worker (any OS)
│   └── start-windows.bat     # Windows batch helper
├── third_party/
│   └── nlohmann/json.hpp     # JSON parser
├── CMakeLists.txt
└── README.md
```

## License

GPLv3. See `LICENSE`.
