#!/usr/bin/env python3
"""
Puzzle #135 Pool — Universal Installer
One script. Any OS. Works interactively OR headless (Vast.ai / CI).

  Interactive:  curl -fsSL https://starnetlive.space/install-135.sh | bash
  Vast.ai:      set WORKER_NAME env var → fully automatic, no prompts
"""

import json
import os
import platform
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

# ─── Config ───────────────────────────────────────────────────────────
VERSION      = "2.0.0"
POOL_JLP     = "jlp://pool.starnetlive.space:5678"
GH_RELEASE   = "https://github.com/Soumya001/starminer/releases/latest/download"
APP_NAME     = "StarMiner135"

# Headless mode: set WORKER_NAME env var to skip all prompts (Vast.ai / Docker)
HEADLESS     = bool(os.environ.get("WORKER_NAME", "").strip())
WORKER_ENV   = os.environ.get("WORKER_NAME", "").strip()

# ─── Colors ───────────────────────────────────────────────────────────
C = {
    "r": "\033[91m", "g": "\033[92m", "y": "\033[93m",
    "b": "\033[94m", "c": "\033[96m", "w": "\033[97m", "x": "\033[0m",
    "bold": "\033[1m", "dim": "\033[2m",
}
if platform.system() == "Windows":
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleMode(ctypes.windll.kernel32.GetStdHandle(-11), 7)
    except Exception:
        C = {k: "" for k in C}

# ─── Platform ─────────────────────────────────────────────────────────
IS_WIN   = platform.system() == "Windows"
IS_MAC   = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"

if IS_WIN:
    INSTALL_DIR  = Path.home() / "Puzzle135"
    BINARY_NAME  = "starminer.exe"
else:
    INSTALL_DIR  = Path.home() / ".puzzle135"
    BINARY_NAME  = "starminer"

BINARY_PATH  = INSTALL_DIR / BINARY_NAME
CONFIG_PATH  = INSTALL_DIR / "config.json"

# ─── UI ───────────────────────────────────────────────────────────────
def banner():
    mode = f"{C['y']}[HEADLESS — Vast.ai]{C['x']} " if HEADLESS else ""
    print(f"""
{C['c']}{'═'*55}{C['x']}
{C['bold']}  Puzzle #135 Pool  {C['dim']}—{C['x']}{C['bold']}  StarMiner v{VERSION}  {mode}{C['x']}
{C['c']}{'─'*55}{C['x']}
  Prize: ~13.5 BTC  |  Algorithm: Pollard's Kangaroo
{C['c']}{'═'*55}{C['x']}
""")

def ok(msg):   print(f"{C['g']}[+]{C['x']} {msg}", flush=True)
def info(msg): print(f"{C['b']}[*]{C['x']} {msg}", flush=True)
def warn(msg): print(f"{C['y']}[!]{C['x']} {msg}", flush=True)
def err(msg):  print(f"{C['r']}[✗]{C['x']} {msg}", flush=True)

def ask(prompt, default=""):
    if HEADLESS:
        return default
    if default:
        print(f"{C['dim']}{prompt} [{default}]:{C['x']}", end=" ", flush=True)
    else:
        print(f"{C['w']}{prompt}:{C['x']}", end=" ", flush=True)
    try:
        val = input().strip()
    except EOFError:
        val = ""
    return val if val else default

def ask_required(prompt):
    while True:
        val = ask(prompt)
        if val:
            return val
        warn("This field is required.")

# ─── GPU detection ────────────────────────────────────────────────────
def detect_gpus():
    nvidia, amd = [], []
    if shutil.which("nvidia-smi"):
        try:
            out = subprocess.check_output(["nvidia-smi", "-L"], text=True,
                                          stderr=subprocess.DEVNULL)
            nvidia = [str(i) for i, l in enumerate(out.splitlines())
                      if l.startswith("GPU ")]
        except Exception:
            pass
    if not nvidia:
        for path in ["/proc/bus/pci/devices", "/sys/class/drm"]:
            if Path(path).exists():
                try:
                    out = subprocess.check_output(["lspci"], text=True,
                                                  stderr=subprocess.DEVNULL)
                    amd = [l for l in out.splitlines()
                           if "AMD" in l and ("Radeon" in l or "Navi" in l)]
                except Exception:
                    pass
                break
    return {"nvidia": nvidia, "amd": amd}

# ─── Binary selection ─────────────────────────────────────────────────
def select_binary(gpu_info):
    """Return (download_url, label) for the best available binary."""
    if IS_WIN:
        if gpu_info["nvidia"]:
            return f"{GH_RELEASE}/starminer-windows-x64-cuda.exe",   "Windows CUDA (NVIDIA)"
        if gpu_info["amd"]:
            return f"{GH_RELEASE}/starminer-windows-x64-hip.exe",    "Windows HIP (AMD)"
        return     f"{GH_RELEASE}/starminer-windows-x64-cpu.exe",    "Windows CPU"
    if IS_MAC:
        return     f"{GH_RELEASE}/starminer-macos-arm64",            "macOS Apple Silicon"
    # Linux
    if gpu_info["nvidia"]:
        return     f"{GH_RELEASE}/starminer-linux-x64-cuda",         "Linux CUDA (NVIDIA)"
    if gpu_info["amd"]:
        return     f"{GH_RELEASE}/starminer-linux-x64-rocm",         "Linux ROCm (AMD)"
    return         f"{GH_RELEASE}/starminer-linux-x64-cpu",          "Linux CPU"

# ─── Download ─────────────────────────────────────────────────────────
def download_binary(url, dest: Path):
    info(f"Downloading {url.split('/')[-1]} ...")
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        req = urllib.request.Request(
            url,
            headers={"User-Agent": f"StarMiner-Installer/{VERSION}"},
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            dest.write_bytes(resp.read())
        if not IS_WIN:
            dest.chmod(0o755)
        ok(f"Saved to {dest}  ({dest.stat().st_size:,} bytes)")
        return True
    except Exception as e:
        err(f"Download failed: {e}")
        return False

# ─── CUDA runtime auto-install ────────────────────────────────────────
def _ensure_cuda_runtime():
    """Check for libcudart.so.12 and install it automatically if missing."""
    import glob
    # Check common locations
    found = (
        glob.glob("/usr/local/cuda*/lib64/libcudart.so*") or
        glob.glob("/usr/lib/x86_64-linux-gnu/libcudart.so*") or
        glob.glob("/usr/lib/libcudart.so*")
    )
    if found:
        return  # already present

    info("CUDA runtime (libcudart.so.12) not found — installing automatically...")

    pkg_managers = []
    if shutil.which("apt-get"):
        pkg_managers.append(("apt-get", [
            ["apt-get", "install", "-y", "-qq", "libcudart-12-0"],
            ["apt-get", "install", "-y", "-qq", "cuda-cudart-12-0"],
            ["apt-get", "install", "-y", "-qq", "cuda-libraries-12-0"],
        ]))
    elif shutil.which("yum"):
        pkg_managers.append(("yum", [
            ["yum", "install", "-y", "-q", "cuda-cudart-12-0"],
        ]))

    installed = False
    for pm_name, attempts in pkg_managers:
        for cmd in attempts:
            try:
                result = subprocess.run(
                    cmd, capture_output=True, timeout=120
                )
                if result.returncode == 0:
                    ok(f"CUDA runtime installed via {pm_name}")
                    installed = True
                    break
            except Exception:
                pass
        if installed:
            break

    if not installed:
        # Try adding NVIDIA CUDA repo and retry
        info("Trying NVIDIA CUDA repository...")
        try:
            subprocess.run(["apt-get", "install", "-y", "-qq", "wget"], capture_output=True, timeout=30)
            subprocess.run([
                "bash", "-c",
                "wget -q https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb -O /tmp/cuda-keyring.deb "
                "&& dpkg -i /tmp/cuda-keyring.deb "
                "&& apt-get update -qq "
                "&& apt-get install -y -qq libcudart-12-0"
            ], timeout=180)
            ok("CUDA runtime installed via NVIDIA repo")
            installed = True
        except Exception as e:
            warn(f"Could not auto-install CUDA runtime: {e}")
            warn("Run manually: apt-get install -y libcudart-12-0")
            warn("Or use the CPU binary: starminer-linux-x64-cpu")

# ─── Desktop / service helpers ────────────────────────────────────────
def create_linux_desktop(cmd):
    apps_dir = Path.home() / ".local" / "share" / "applications"
    apps_dir.mkdir(parents=True, exist_ok=True)
    desktop = apps_dir / f"{APP_NAME}.desktop"
    desktop.write_text(f"""[Desktop Entry]
Name={APP_NAME}
Comment=Puzzle #135 Kangaroo Pool Worker
Exec={cmd}
Type=Application
Terminal=true
Icon=utilities-terminal
""")
    desktop.chmod(0o755)
    ok(f"App menu entry: {desktop}")
    d = Path.home() / "Desktop"
    if d.exists():
        shutil.copy2(desktop, d / f"{APP_NAME}.desktop")
        (d / f"{APP_NAME}.desktop").chmod(0o755)
        ok(f"Desktop shortcut: {d / APP_NAME}.desktop")

def create_macos_command(cmd):
    d = Path.home() / "Desktop"
    d.mkdir(exist_ok=True)
    p = d / f"{APP_NAME}.command"
    p.write_text(f"#!/bin/bash\ncd {INSTALL_DIR}\n{cmd}\n")
    p.chmod(0o755)
    ok(f"Desktop command: {p}")

def create_windows_bat(cmd):
    for desktop in [Path.home() / "Desktop",
                    Path.home() / "OneDrive" / "Desktop"]:
        if desktop.exists():
            bat = desktop / f"{APP_NAME}.bat"
            bat.write_text(f'@echo off\ncd /d "{INSTALL_DIR}"\n{cmd}\npause\n')
            ok(f"Desktop shortcut: {bat}")
            return
    bat = INSTALL_DIR / f"{APP_NAME}.bat"
    bat.write_text(f'@echo off\ncd /d "{INSTALL_DIR}"\n{cmd}\npause\n')
    ok(f"Batch file: {bat}")

def install_systemd_service(cmd):
    sd = Path.home() / ".config" / "systemd" / "user"
    sd.mkdir(parents=True, exist_ok=True)
    svc = sd / f"{APP_NAME}.service"
    svc.write_text(f"""[Unit]
Description=Puzzle #135 Kangaroo Pool Worker
After=network.target

[Service]
Type=simple
WorkingDirectory={INSTALL_DIR}
ExecStart={cmd}
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
""")
    ok(f"Systemd service: {svc}")
    info("Enable with:")
    print(f"  systemctl --user enable --now {APP_NAME}.service")
    print(f"  journalctl --user -u {APP_NAME}.service -f")

# ─── Main ─────────────────────────────────────────────────────────────
def main():
    banner()

    # ── Detect GPU ────────────────────────────────────────────────────
    info("Detecting GPU...")
    gpu_info = detect_gpus()
    if gpu_info["nvidia"]:
        ok(f"NVIDIA GPU(s) detected: {', '.join(gpu_info['nvidia'])}")
    elif gpu_info["amd"]:
        warn("AMD GPU detected — will use ROCm/HIP binary")
    else:
        warn("No discrete GPU found — will use CPU binary (slower)")

    # ── Select binary ─────────────────────────────────────────────────
    dl_url, dl_label = select_binary(gpu_info)
    ok(f"Selected: {dl_label}")

    pool_url = POOL_JLP

    # ── Already installed? ────────────────────────────────────────────
    if BINARY_PATH.exists() and not HEADLESS:
        warn(f"StarMiner already installed at {INSTALL_DIR}")
        choice = ask("  [R]einstall  [U]ninstall  [C]ancel", "R").upper()
        if choice == "U":
            shutil.rmtree(INSTALL_DIR, ignore_errors=True)
            ok("Uninstalled.")
            return
        elif choice == "C":
            return
        info("Reinstalling...")
        BINARY_PATH.unlink(missing_ok=True)

    # ── Worker identity ───────────────────────────────────────────────
    # Load existing config to preserve device_id across reinstalls
    saved_wallet = ""
    saved_device = ""
    if CONFIG_PATH.exists():
        try:
            cfg = json.loads(CONFIG_PATH.read_text())
            saved_wallet = cfg.get("wallet", "")
            saved_device = cfg.get("device_id", "")
        except Exception:
            pass

    # Stable device ID: generated once, never changes for this machine
    if not saved_device:
        import uuid
        saved_device = uuid.uuid4().hex[:12]

    if HEADLESS:
        wallet = WORKER_ENV or saved_wallet or f"worker-{os.uname().nodename}"
        ok(f"Wallet from env: {wallet}")
    else:
        print(f"\n{C['bold']}Enter your Bitcoin address for payouts.{C['x']}")
        print(f"{C['dim']}Each device gets a unique ID — multiple machines with the same wallet are tracked separately.{C['x']}\n")
        default_wallet = saved_wallet or ""
        wallet = ask_required(f"BTC address{' ['+default_wallet+']' if default_wallet else ''}")
        if not wallet and default_wallet:
            wallet = default_wallet

    # worker_name = wallet/deviceid (unique per device, payouts group by wallet)
    worker_name = f"{wallet}/{saved_device}"
    ok(f"Wallet:    {wallet}")
    ok(f"Device ID: {saved_device}  (stable, survives reinstalls)")
    ok(f"Worker ID: {worker_name}")

    # ── Download binary ───────────────────────────────────────────────
    if not download_binary(dl_url, BINARY_PATH):
        # AMD HIP fallback to CPU on Windows if HIP binary not yet released
        if IS_WIN and gpu_info["amd"]:
            warn("HIP binary not available yet — falling back to CPU")
            dl_url = f"{GH_RELEASE}/starminer-windows-x64-cpu.exe"
            if not download_binary(dl_url, BINARY_PATH):
                sys.exit(1)
        else:
            sys.exit(1)

    # ── CUDA runtime check + auto-install ────────────────────────────
    if IS_LINUX and gpu_info.get("nvidia"):
        _ensure_cuda_runtime()

    # ── Save config ───────────────────────────────────────────────────
    CONFIG_PATH.write_text(json.dumps({
        "worker_name":  worker_name,
        "wallet":       wallet,
        "device_id":    saved_device,
        "pool_url":     pool_url,
        "binary":       str(BINARY_PATH),
        "installed_at": time.time(),
        "version":      VERSION,
    }, indent=2))
    ok(f"Config: {CONFIG_PATH}")

    # ── Build run command ─────────────────────────────────────────────
    binary = f'"{BINARY_PATH}"' if IS_WIN and " " in str(BINARY_PATH) else str(BINARY_PATH)
    cmd = f'{binary} --pool {pool_url} --worker {worker_name} --gpus all'

    # ── Desktop / service setup (skip in headless) ────────────────────
    if not HEADLESS:
        print(f"\n{C['bold']}Creating shortcut...{C['x']}")
        if IS_LINUX:   create_linux_desktop(cmd)
        elif IS_MAC:   create_macos_command(cmd)
        elif IS_WIN:   create_windows_bat(cmd)

        if IS_LINUX and shutil.which("systemctl"):
            print(f"\n{C['bold']}Background service (optional):{C['x']}")
            if ask("Install systemd user service? [y/N]", "N").lower() == "y":
                install_systemd_service(cmd)

    # ── Summary ───────────────────────────────────────────────────────
    print(f"\n{C['g']}{'═'*55}{C['x']}")
    print(f"{C['bold']}  {'Ready!' if HEADLESS else 'Installation complete!'}{C['x']}")
    print(f"{C['g']}{'─'*55}{C['x']}")
    print(f"  Worker : {C['c']}{worker_name}{C['x']}")
    print(f"  Pool   : {C['c']}{pool_url}{C['x']}")
    print(f"  Binary : {C['c']}{BINARY_PATH}{C['x']}")
    print(f"{C['g']}{'═'*55}{C['x']}\n")

    # ── Start ─────────────────────────────────────────────────────────
    if HEADLESS:
        info("Starting StarMiner (headless mode — logs below)...")
        print(f"{C['dim']}{'─'*55}{C['x']}\n", flush=True)
        os.execv(str(BINARY_PATH), [
            str(BINARY_PATH),
            "--pool", pool_url,
            "--worker", worker_name,
            "--gpus", "all",
        ])
    else:
        start = ask("Start mining now? [Y/n]", "Y").lower()
        if start in ("y", "yes", ""):
            info("Starting StarMiner...")
            print(f"{C['dim']}{'─'*55}{C['x']}\n", flush=True)
            try:
                os.chdir(INSTALL_DIR)
                subprocess.run(cmd, shell=True)
            except KeyboardInterrupt:
                pass

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{C['y']}[!] Cancelled.{C['x']}")
        sys.exit(0)
    except Exception as e:
        import traceback
        print(f"\n{C['r']}[✗] Installer crashed: {e}{C['x']}")
        traceback.print_exc()
        sys.exit(1)
