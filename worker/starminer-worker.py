#!/usr/bin/env python3
"""
StarMiner Universal Worker — One file, any device, any GPU.
Auto-downloads the right binary, shows live terminal dashboard.
"""

import argparse
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

VERSION = "1.0.0"
DEFAULT_POOL = "https://starnetlive.space"
APP_NAME = "StarMiner"

IS_WIN = platform.system() == "Windows"
IS_MAC = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"
ARCH = platform.machine().lower()

if IS_WIN:
    INSTALL_DIR = Path("C:/StarMiner")
    BINARY_NAME = "starminer.exe"
else:
    INSTALL_DIR = Path.home() / ".starminer"
    BINARY_NAME = "starminer"

BINARY_PATH = INSTALL_DIR / BINARY_NAME
CONFIG_PATH = INSTALL_DIR / "worker.json"
LOG_PATH = INSTALL_DIR / "worker.log"

# ANSI colors
C = {
    "reset": "\033[0m",
    "bold": "\033[1m",
    "dim": "\033[2m",
    "green": "\033[32m",
    "red": "\033[31m",
    "yellow": "\033[33m",
    "blue": "\033[34m",
    "cyan": "\033[36m",
    "white": "\033[37m",
    "bg_green": "\033[42m",
    "bg_red": "\033[41m",
    "bg_blue": "\033[44m",
    "clear": "\033[2J\033[H",
}

if IS_WIN:
    # Enable ANSI on Windows
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
    except Exception:
        # Fallback: strip ANSI codes
        C = {k: "" for k in C}
        C["clear"] = "\n" * 50


class LiveState:
    def __init__(self):
        self.lock = threading.Lock()
        self.connected = False
        self.status = "Initializing..."
        self.uptime = 0
        self.reconnects = 0
        self.puzzle = ""
        self.chunk_id = ""
        self.range_start = ""
        self.range_end = ""
        self.dp_bits = 28
        self.gpus = []
        self.total_speed = 0.0
        self.local_dps = 0
        self.sent_dps = 0
        self.your_dps = 0
        self.pool_total = 0
        self.your_share = 0.0
        self.submission_rate = 0.0
        self.log_lines = []
        self.running = True
        self.start_time = time.time()

    def add_log(self, msg):
        with self.lock:
            ts = time.strftime("%H:%M:%S")
            line = f"[{ts}] {msg}"
            self.log_lines.append(line)
            if len(self.log_lines) > 20:
                self.log_lines.pop(0)
            # Also write to file
            try:
                with open(LOG_PATH, "a") as f:
                    f.write(line + "\n")
            except Exception:
                pass


STATE = LiveState()


def _log(msg):
    STATE.add_log(msg)


def detect_platform() -> str:
    """Detect platform + GPU for binary selection."""
    if IS_MAC:
        if ARCH in ("arm64", "aarch64"):
            return "macos-arm64"
        return "macos-x64"

    if IS_WIN:
        if _has_nvidia():
            return "windows-x64-cuda"
        return "windows-x64-cpu"

    if IS_LINUX:
        if _has_nvidia():
            return "linux-x64-cuda"
        return "linux-x64-cpu"

    return "linux-x64-cpu"


def _has_nvidia() -> bool:
    """Check if NVIDIA GPU is present."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "-L"],
            capture_output=True,
            text=True,
            timeout=5
        )
        return result.returncode == 0 and "GPU" in result.stdout
    except Exception:
        return False


def detect_gpus() -> list[dict]:
    """Detect available GPUs and their info."""
    gpus = []
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=index,name,memory.total,temperature.gpu,utilization.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True,
            text=True,
            timeout=5
        )
        if result.returncode == 0:
            for line in result.stdout.strip().split("\n"):
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 5:
                    gpus.append({
                        "index": int(parts[0]),
                        "name": parts[1],
                        "memory_mb": int(float(parts[2])),
                        "temp_c": int(float(parts[3])) if parts[3] else 0,
                        "util_pct": int(float(parts[4])) if parts[4] else 0,
                    })
    except Exception:
        pass

    if not gpus and IS_MAC:
        # macOS - detect Apple Silicon GPU
        try:
            result = subprocess.run(
                ["system_profiler", "SPDisplaysDataType", "-json"],
                capture_output=True,
                text=True,
                timeout=5
            )
            if result.returncode == 0:
                data = json.loads(result.stdout)
                for item in data.get("SPDisplaysDataType", []):
                    gpus.append({
                        "index": 0,
                        "name": item.get("sppci_model", "Apple GPU"),
                        "memory_mb": 0,
                        "temp_c": 0,
                        "util_pct": 0,
                    })
        except Exception:
            gpus.append({"index": 0, "name": "Apple Silicon", "memory_mb": 0, "temp_c": 0, "util_pct": 0})

    if not gpus:
        # CPU fallback
        gpus.append({"index": 0, "name": "CPU Fallback", "memory_mb": 0, "temp_c": 0, "util_pct": 0})

    return gpus


def ensure_binary(pool_url: str) -> bool:
    """Download the right starminer binary for this platform."""
    if BINARY_PATH.exists():
        size = BINARY_PATH.stat().st_size
        if size > 10_000_000:
            _log(f"Binary found: {BINARY_PATH} ({size:,} bytes)")
            return True

    plat = detect_platform()
    url = f"{pool_url}/download/starminer-{plat}"
    if IS_WIN:
        url += ".exe"

    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    _log(f"Downloading StarMiner for {plat}...")
    _log(f"URL: {url}")

    try:
        req = urllib.request.Request(url)
        req.add_header("User-Agent", f"StarMiner-Worker/{VERSION}")
        with urllib.request.urlopen(req, timeout=120) as resp:
            with open(BINARY_PATH, "wb") as f:
                f.write(resp.read())
        if not IS_WIN:
            BINARY_PATH.chmod(0o755)
        size = BINARY_PATH.stat().st_size
        _log(f"Downloaded {size:,} bytes")
        return True
    except Exception as e:
        _log(f"Download failed: {e}")
        return False


def load_or_create_config(pool_url: str) -> dict:
    """Load or interactively create worker config."""
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH) as f:
                return json.load(f)
        except Exception:
            pass

    print("\n" + "=" * 60)
    print("  ⭐  Welcome to StarMiner Worker Setup")
    print("=" * 60)
    print(f"\nDetected platform: {detect_platform()}")
    gpus = detect_gpus()
    print(f"Detected GPUs ({len(gpus)}):")
    for g in gpus:
        print(f"  [{g['index']}] {g['name']}")

    print(f"\nPool server: {pool_url}")
    worker = input("\nEnter your Bitcoin address (worker name): ").strip()
    if not worker:
        worker = f"worker-{platform.node()}"

    gpu_input = input(f"GPU IDs to use (comma-separated, default: all): ").strip()
    if gpu_input:
        gpu_ids = gpu_input
    else:
        gpu_ids = ",".join(str(g["index"]) for g in gpus)

    config = {
        "pool_url": pool_url,
        "worker_name": worker,
        "gpu_ids": gpu_ids,
        "version": VERSION,
    }
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        json.dump(config, f, indent=2)
    print(f"\n✅ Config saved to {CONFIG_PATH}")
    print("Run again to start mining!\n")
    return config


def parse_starminer_line(line: str):
    """Parse starminer stdout for live stats."""
    # Speed: 525.00 MKeys/s | Local DPs: 31 | Sent: 32 | Your total: 32 | Pool total: 32 | Your share: 100.00%
    speed_match = re.search(r"Speed:\s*([\d.]+)\s*MKeys/s", line)
    if speed_match:
        STATE.total_speed = float(speed_match.group(1))

    dp_match = re.search(
        r"Local DPs:\s*(\d+)\s*\|\s*Sent:\s*(\d+)\s*\|\s*Your total:\s*(\d+)\s*\|\s*Pool total:\s*(\d+)\s*\|\s*Your share:\s*([\d.]+)%",
        line
    )
    if dp_match:
        STATE.local_dps = int(dp_match.group(1))
        STATE.sent_dps = int(dp_match.group(2))
        STATE.your_dps = int(dp_match.group(3))
        STATE.pool_total = int(dp_match.group(4))
        STATE.your_share = float(dp_match.group(5))

    # GPU 0: allocated 2707 MB, 917504 kangaroos. OldGpuMode: Yes
    gpu_match = re.search(r"GPU\s*(\d+):\s*allocated\s*(\d+)\s*MB", line)
    if gpu_match:
        idx = int(gpu_match.group(1))
        mem = int(gpu_match.group(2))
        found = False
        for g in STATE.gpus:
            if g.get("index") == idx:
                g["memory_used_mb"] = mem
                found = True
                break
        if not found:
            STATE.gpus.append({"index": idx, "memory_used_mb": mem, "name": f"GPU {idx}"})

    # [PoolManager] Received new work: Puzzle #135 (DP bits: 28)
    work_match = re.search(r"Received new work:\s*(.+?)\s*\(DP bits:\s*(\d+)\)", line)
    if work_match:
        STATE.puzzle = work_match.group(1).strip()
        STATE.dp_bits = int(work_match.group(2))

    # [+] Work assigned: Puzzle #135
    #     DP Bits: 28
    #     Work ID: 12345
    chunk_match = re.search(r"Work ID:\s*(\d+)", line)
    if chunk_match:
        STATE.chunk_id = chunk_match.group(1)

    # Connection status
    if "Connected to pool successfully" in line:
        STATE.connected = True
        STATE.status = "🟢 Online"
    elif "Failed to connect" in line or "Authentication failed" in line:
        STATE.connected = False
        STATE.status = "🔴 Disconnected"
        STATE.reconnects += 1


def draw_dashboard():
    """Draw the live terminal dashboard."""
    while STATE.running:
        time.sleep(1)
        with STATE.lock:
            lines = []
            w = 68

            def box(title, content_lines):
                lines.append("╔" + "═" * w + "╗")
                pad = (w - len(title)) // 2
                lines.append("║" + " " * pad + C["bold"] + title + C["reset"] + " " * (w - pad - len(title)) + "║")
                lines.append("╠" + "═" * w + "╣")
                for cl in content_lines:
                    cl = cl[:w]
                    lines.append("║ " + cl + " " * (w - 1 - len(cl)) + "║")
                lines.append("╚" + "═" * w + "╝")

            uptime = int(time.time() - STATE.start_time)
            uptime_str = f"{uptime//3600:02d}:{(uptime%3600)//60:02d}:{uptime%60:02d}"

            # Header
            header = [
                f"{APP_NAME} Worker v{VERSION}  |  Pool: {STATE.pool_url}",
                f"Worker: {STATE.worker_name[:40]}"
            ]
            box("⭐  STARMINER WORKER", header)

            # Connection
            conn = [
                f"Status: {STATE.status}  |  Uptime: {uptime_str}  |  Reconnects: {STATE.reconnects}",
            ]
            box("CONNECTION", conn)

            # Work
            work = [
                f"Puzzle: {STATE.puzzle or 'Waiting...'}  |  Chunk: {STATE.chunk_id or '—'}",
                f"DP Bits: {STATE.dp_bits}",
            ]
            box("CURRENT WORK", work)

            # GPUs
            gpu_lines = []
            for g in STATE.gpus:
                name = g.get("name", "Unknown")
                mem = g.get("memory_used_mb", 0)
                temp = g.get("temp_c", 0)
                util = g.get("util_pct", 0)
                temp_str = f"{temp}°C" if temp else ""
                gpu_lines.append(f"[{g['index']}] {name[:30]:30} 🟢  {temp_str:6}")
                if mem:
                    gpu_lines.append(f"      Memory: {mem} MB")
            if not gpu_lines:
                gpu_lines.append("No GPU info yet...")
            box(f"GPUs ({len(STATE.gpus)} detected)", gpu_lines)

            # Stats
            stats = [
                f"Speed: {STATE.total_speed:,.2f} MKeys/s",
                f"Your DPs: {STATE.your_dps:,}  |  Pool Total: {STATE.pool_total:,}",
                f"Your Share: {STATE.your_share:.2f}%  |  Sent: {STATE.sent_dps:,}",
            ]
            box("POOL STATS", stats)

            # Log
            log = STATE.log_lines[-8:] if STATE.log_lines else ["Starting..."]
            box("LOG", log)

        # Render
        output = C["clear"] + "\n".join(lines)
        print(output, end="", flush=True)


def run_starminer(pool_url: str, worker: str, gpu_ids: str):
    """Run the starminer binary and parse its output."""
    cmd = [
        str(BINARY_PATH),
        "--pool", pool_url,
        "--worker", worker,
        "--gpus", gpu_ids,
    ]
    _log(f"Starting: {' '.join(cmd)}")

    si = None
    if IS_WIN:
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        startupinfo=si,
        creationflags=0x08000000 if IS_WIN else 0,
        text=True,
        bufsize=1,
    )

    try:
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                parse_starminer_line(line)
    except KeyboardInterrupt:
        _log("Interrupted — stopping starminer...")
        proc.terminate()
        proc.wait(timeout=5)
        if proc.poll() is None:
            proc.kill()
        raise

    proc.wait()
    return proc.returncode


def main():
    parser = argparse.ArgumentParser(
        description=f"StarMiner Universal Worker v{VERSION}"
    )
    parser.add_argument("--pool", default=DEFAULT_POOL, help="Pool server URL")
    parser.add_argument("--worker", default="", help="Worker name (Bitcoin address)")
    parser.add_argument("--gpus", default="", help="GPU IDs (comma-separated)")
    parser.add_argument("--setup", action="store_true", help="Force re-run setup")
    args = parser.parse_args()

    STATE.pool_url = args.pool.rstrip("/")

    # Setup or load config
    if args.setup or (not args.worker and not CONFIG_PATH.exists()):
        config = load_or_create_config(STATE.pool_url)
        if not args.worker:
            return 0
    elif CONFIG_PATH.exists():
        with open(CONFIG_PATH) as f:
            config = json.load(f)
    else:
        config = {"pool_url": STATE.pool_url, "worker_name": f"worker-{platform.node()}", "gpu_ids": "0"}

    worker_name = args.worker or config.get("worker_name", f"worker-{platform.node()}")
    gpu_ids = args.gpus or config.get("gpu_ids", "0")
    STATE.worker_name = worker_name

    print(f"\n{'='*60}")
    print(f"  ⭐  Starting {APP_NAME} Worker v{VERSION}")
    print(f"  Pool: {STATE.pool_url}")
    print(f"  Worker: {worker_name}")
    print(f"  GPUs: {gpu_ids}")
    print(f"{'='*60}\n")

    # Download binary
    if not ensure_binary(STATE.pool_url):
        print("❌ Failed to download starminer binary.")
        print("   Make sure the pool server has binaries for your platform.")
        sys.exit(1)

    # Detect GPUs for display
    STATE.gpus = detect_gpus()

    # Start dashboard thread
    STATE.running = True
    dash_thread = threading.Thread(target=draw_dashboard, daemon=True)
    dash_thread.start()

    # Run starminer with auto-restart
    while STATE.running:
        try:
            ret = run_starminer(STATE.pool_url, worker_name, gpu_ids)
            _log(f"starminer exited with code {ret}")
            if ret != 0:
                STATE.reconnects += 1
                STATE.status = "🟡 Restarting..."
                _log("Restarting in 5 seconds...")
                time.sleep(5)
        except KeyboardInterrupt:
            STATE.running = False
            _log("Shutdown requested.")
            break
        except Exception as e:
            _log(f"Error: {e}")
            STATE.reconnects += 1
            time.sleep(10)

    print("\n👋 StarMiner Worker stopped.\n")


if __name__ == "__main__":
    main()
