# StarMiner

**StarMiner** is a GPU-accelerated Bitcoin puzzle solver with native support for custom HTTP/JSON pools. Forked and heavily improved from theCollider, it connects miners directly to your own pool server without requiring the JLP binary protocol.

## Features

- **HTTP/JSON Pool Mode** — Connect to any HTTP/HTTPS pool server with `--pool https://yourserver.com`
- **True DP Sharing** — Workers submit distinguished points to a central pool for collision detection
- **Multi-GPU** — Auto-detect and use all CUDA GPUs (`--gpus 0,1,2,3`)
- **Async DP Batching** — Background thread batches and flushes DPs efficiently
- **Heartbeat & Retry** — Exponential backoff reconnect, periodic keepalive pings
- **Per-GPU Telemetry** — Reports hash rates per device to the pool server
- **Built on RCKangaroo** — ~8 GKeys/s per RTX 4090 using RetiredCoder's kernel

## Quick Start

### Build

```bash
mkdir build && cd build
cmake .. -DCOLLIDER_USE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86
make starminer -j$(nproc)
```

### Run Solo

```bash
./starminer --puzzle 135 --kangaroo --gpus 0
```

### Run in Pool Mode

```bash
./starminer --pool https://starnetlive.space --worker YOUR_BTC_ADDRESS --gpus 0,1,2,3
```

## Pool Server API

StarMiner speaks a simple JSON/REST protocol to the pool server:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/pool/auth` | POST | Worker authentication |
| `/api/v1/pool/work` | POST | Request work chunk |
| `/api/v1/pool/dp_batch` | POST | Submit distinguished points |
| `/api/v1/pool/solution` | POST | Report found key |
| `/api/v1/pool/stats` | GET | Pool statistics |
| `/api/v1/pool/ping` | POST | Heartbeat keepalive |

A reference Python/FastAPI implementation is available in the `puzzle-135-pool` companion repo.

## What Was Changed From Upstream

1. **Added `HttpPoolClient`** — New C++ pool client using libcurl + JSON/REST
2. **Added nlohmann/json** — Proper JSON parsing instead of manual string hacks
3. **Added heartbeat thread** — Keeps workers marked as active on the server
4. **Added retry logic** — Exponential backoff with jitter for all HTTP requests
5. **Added GPU telemetry** — Per-device hash rate reporting in DP batches
6. **Rebranded to StarMiner** — New binary name, banners, config path (`~/.starminer/`)
7. **Removed collisionprotocol.com lock-in** — JLP preserved as fallback, HTTP is first-class

## License

GPLv3 (inherits from upstream and RCKangaroo). See `LICENSE`.

## Disclaimer

This is a lottery pool. Expected time to find a 134-bit key is astronomically high. Participate for fun/education, not guaranteed profit.
