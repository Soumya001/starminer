#!/usr/bin/env python3
"""
Puzzle #135 Pool — Universal Installer
One script. Any OS. Terminal-first.
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
VERSION = "1.0.0"
POOL_URL_DEFAULT = "https://starnetlive.space"
WORKER_URL = "https://raw.githubusercontent.com/Soumya001/puzzle-135-pool/main/worker/puzzle135_worker.py"
APP_NAME = "Puzzle135Worker"

# ─── Colors ───────────────────────────────────────────────────────────
C = {
    "r": "\033[91m", "g": "\033[92m", "y": "\033[93m",
    "b": "\033[94m", "c": "\033[96m", "w": "\033[97m", "x": "\033[0m",
    "bold": "\033[1m", "dim": "\033[2m",
}

if platform.system() == "Windows":
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
    except Exception:
        C = {k: "" for k in C}

# ─── Platform helpers ─────────────────────────────────────────────────
IS_WIN = platform.system() == "Windows"
IS_MAC = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"
ARCH = platform.machine().lower()

if IS_WIN:
    INSTALL_DIR = Path.home() / "Puzzle135"
    BINARY_NAME = "collider.exe"
else:
    INSTALL_DIR = Path.home() / ".puzzle135"
    BINARY_NAME = "collider"

WORKER_PATH = INSTALL_DIR / "puzzle135_worker.py"
CONFIG_PATH = INSTALL_DIR / "worker.json"
BINARY_PATH = INSTALL_DIR / BINARY_NAME

# ─── UI helpers ───────────────────────────────────────────────────────
def banner():
    print(f"""
{C['c']}{'═'*55}{C['x']}
{C['bold']}  Puzzle #135 Pool  {C['dim']}—{C['x']}{C['bold']}  Universal Installer v{VERSION}{C['x']}
{C['c']}{'─'*55}{C['x']}
  Prize: ~13.5 BTC  |  Algorithm: Pollard's Kangaroo
{C['c']}{'═'*55}{C['x']}
""")

def ok(msg):   print(f"{C['g']}[+] {C['x']}{msg}")
def info(msg): print(f"{C['b']}[*] {C['x']}{msg}")
def warn(msg): print(f"{C['y']}[!] {C['x']}{msg}")
def err(msg):  print(f"{C['r']}[✗] {C['x']}{msg}")

def ask(prompt, default=""):
    if default:
        print(f"{C['dim']}{prompt} [{default}]:{C['x']}", end=" ")
    else:
        print(f"{C['w']}{prompt}:{C['x']}", end=" ")
    try:
        val = input().strip()
    except EOFError:
        val = ""
    return val if val else default

def ask_required(prompt):
    while True:
        print(f"{C['w']}{prompt}:{C['x']}", end=" ")
        val = input().strip()
        if val:
            return val
        err("Required. Please enter a value.")

# ─── Core ─────────────────────────────────────────────────────────────
def detect_gpus():
    """Try to detect GPUs and determine backend type."""
    nvidia_gpus = []
    amd_gpus = []
    intel_gpus = []

    # NVIDIA detection
    if shutil.which("nvidia-smi"):
        try:
            out = subprocess.check_output(["nvidia-smi", "-L"], text=True, stderr=subprocess.DEVNULL)
            for i, line in enumerate(out.strip().split("\n")):
                if line.startswith("GPU "):
                    nvidia_gpus.append(str(i))
        except Exception:
            pass

    # AMD detection (Linux)
    if IS_LINUX and shutil.which("lspci"):
        try:
            out = subprocess.check_output(["lspci"], text=True, stderr=subprocess.DEVNULL)
            for line in out.split("\n"):
                if "VGA" in line or "Display" in line:
                    if "AMD" in line or "ATI" in line:
                        amd_gpus.append(line.strip())
                    elif "Intel" in line and "Arc" in line:
                        intel_gpus.append(line.strip())
        except Exception:
            pass

    return {
        "nvidia": nvidia_gpus,
        "amd": amd_gpus,
        "intel": intel_gpus,
    }

def download_worker():
    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    info(f"Downloading worker to {WORKER_PATH}")
    try:
        urllib.request.urlretrieve(WORKER_URL, str(WORKER_PATH))
        ok(f"Worker saved ({WORKER_PATH.stat().st_size:,} bytes)")
    except Exception as e:
        err(f"Download failed: {e}")
        sys.exit(1)

def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)
    ok(f"Config saved to {CONFIG_PATH}")

def create_linux_desktop(name, cmd, icon_text="🧩"):
    """Create .desktop launcher on Linux."""
    apps_dir = Path.home() / ".local" / "share" / "applications"
    apps_dir.mkdir(parents=True, exist_ok=True)
    desktop_path = apps_dir / f"{APP_NAME}.desktop"
    desktop_content = f"""[Desktop Entry]
Name={name}
Comment=Puzzle #135 Pool Worker
Exec={cmd}
Type=Application
Terminal=true
Icon=utilities-terminal
Categories=Utility;
"""
    desktop_path.write_text(desktop_content)
    desktop_path.chmod(0o755)
    ok(f"Application menu entry: {desktop_path}")

    # Also copy to Desktop if it exists
    desktop_dir = Path.home() / "Desktop"
    if desktop_dir.exists():
        desktop_copy = desktop_dir / f"{APP_NAME}.desktop"
        shutil.copy2(desktop_path, desktop_copy)
        desktop_copy.chmod(0o755)
        ok(f"Desktop shortcut: {desktop_copy}")

def create_macos_command(name, cmd):
    """Create .command file on macOS."""
    desktop_dir = Path.home() / "Desktop"
    desktop_dir.mkdir(exist_ok=True)
    cmd_path = desktop_dir / f"{APP_NAME}.command"
    cmd_content = f"""#!/bin/bash
cd {INSTALL_DIR}
{cmd}
"""
    cmd_path.write_text(cmd_content)
    cmd_path.chmod(0o755)
    ok(f"Desktop command: {cmd_path}")

def create_windows_shortcut(name, cmd):
    """Create .bat file on Windows Desktop."""
    desktop = Path.home() / "Desktop"
    # Handle OneDrive/Desktop or missing Desktop folder
    if not desktop.exists():
        one_drive_desktop = Path.home() / "OneDrive" / "Desktop"
        if one_drive_desktop.exists():
            desktop = one_drive_desktop
        else:
            desktop.mkdir(parents=True, exist_ok=True)
    bat_path = desktop / f"{APP_NAME}.bat"
    bat_path.write_text(f'@echo off\ncd /d "{INSTALL_DIR}"\n{cmd}\npause\n')
    ok(f"Desktop batch: {bat_path}")

def install_linux_service(worker_name, wallet, pool_url, use_cpu=False):
    """Install systemd user service on Linux."""
    systemd_dir = Path.home() / ".config" / "systemd" / "user"
    systemd_dir.mkdir(parents=True, exist_ok=True)
    service_path = systemd_dir / f"{APP_NAME}.service"
    python = sys.executable
    cpu_flag = " --cpu" if use_cpu else ""
    service_content = f"""[Unit]
Description=Puzzle #135 Pool Worker
After=network.target

[Service]
Type=simple
WorkingDirectory={INSTALL_DIR}
ExecStart={python} {WORKER_PATH} --pool {pool_url} --name {worker_name} --wallet {wallet}{cpu_flag}
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
"""
    service_path.write_text(service_content)
    ok(f"Systemd user service: {service_path}")
    info("To start background service:")
    print(f"  systemctl --user enable --now {APP_NAME}.service")
    print(f"  systemctl --user status {APP_NAME}.service")
    print(f"  journalctl --user -u {APP_NAME}.service -f")

def install_windows_service(worker_name, wallet, pool_url, use_cpu=False):
    """Create scheduled task on Windows."""
    cpu_flag = " --cpu" if use_cpu else ""
    ok("To run in background on Windows, use Task Scheduler:")
    print(f"  schtasks /create /tn {APP_NAME} /tr \"{sys.executable} {WORKER_PATH} --pool {pool_url} --name {worker_name} --wallet {wallet}{cpu_flag}\" /sc onlogon /rl highest")

def main():
    banner()

    # ── Step 1: Detect GPUs ───────────────────────────────────────────
    gpu_info = detect_gpus()
    use_cpu = False
    if gpu_info["nvidia"]:
        ok(f"Detected {len(gpu_info['nvidia'])} NVIDIA GPU(s): {', '.join(gpu_info['nvidia'])}")
    elif gpu_info["amd"]:
        warn("AMD GPU detected — CUDA not available.")
        info("StarMiner will use CPU mode (slower but works on any PC).")
        use_cpu = True
    elif gpu_info["intel"]:
        warn("Intel Arc GPU detected — CUDA not available.")
        info("StarMiner will use CPU mode (slower but works on any PC).")
        use_cpu = True
    else:
        warn("No NVIDIA GPU detected. Worker will run on CPU (slower but works on any PC).")
        use_cpu = True

    # ── Step 2: Form ──────────────────────────────────────────────────
    print(f"\n{C['bold']}┌─ Worker Configuration ──────────────────────────────┐{C['x']}")
    print(f"{C['dim']}│  Pool: {POOL_URL_DEFAULT}                              │{C['x']}")
    print(f"{C['dim']}│  Just enter your worker name and BTC payout address. │{C['x']}")
    print(f"{C['bold']}└──────────────────────────────────────────────────────┘{C['x']}\n")

    default_name = f"worker-{platform.node()}"
    worker_name = ask_required("Worker name (or BTC address)")
    wallet = ask("BTC payout address (optional)", worker_name)
    pool_url = POOL_URL_DEFAULT

    print("")

    # ── Step 3: Download ──────────────────────────────────────────────
    download_worker()
    cfg = {
        "worker_name": worker_name,
        "wallet_address": wallet,
        "pool_url": pool_url,
        "installed_at": time.time(),
        "version": VERSION,
    }
    save_config(cfg)

    # ── Step 4: Desktop Launcher ──────────────────────────────────────
    print(f"\n{C['bold']}┌─ Creating Desktop Launcher ─────────────────────────┐{C['x']}")
    py = sys.executable
    wp = str(WORKER_PATH)
    # Quote paths on Windows if they contain spaces
    if IS_WIN:
        if " " in py:
            py = f'"{py}"'
        if " " in wp:
            wp = f'"{wp}"'
    cmd = f"{py} {wp} --pool {pool_url} --name {worker_name}"
    if wallet:
        cmd += f" --wallet {wallet}"
    if use_cpu:
        cmd += " --cpu"

    if IS_LINUX:
        create_linux_desktop(APP_NAME, cmd)
    elif IS_MAC:
        create_macos_command(APP_NAME, cmd)
    elif IS_WIN:
        create_windows_shortcut(APP_NAME, cmd)
    else:
        warn("Unknown OS — no desktop shortcut created.")

    # ── Step 5: Background Service (optional) ─────────────────────────
    print(f"\n{C['bold']}┌─ Background Service (optional) ─────────────────────┐{C['x']}")
    print(f"{C['dim']}│  The desktop shortcut opens a terminal dashboard.   │{C['x']}")
    print(f"{C['dim']}│  You can also run the worker as a background service.│{C['x']}")
    print(f"{C['bold']}└──────────────────────────────────────────────────────┘{C['x']}\n")

    if IS_LINUX and shutil.which("systemctl"):
        install_linux_service(worker_name, wallet, pool_url, use_cpu)
    elif IS_WIN:
        install_windows_service(worker_name, wallet, pool_url, use_cpu)
    else:
        info("Background service setup not available on this OS.")

    # ── Step 6: Start ─────────────────────────────────────────────────
    print(f"\n{C['g']}{'═'*55}{C['x']}")
    print(f"{C['bold']}  Installation complete!{C['x']}")
    print(f"{C['g']}{'═'*55}{C['x']}\n")

    print(f"{C['w']}Next steps:{C['x']}")
    print(f"  1. Double-click the '{APP_NAME}' icon on your desktop")
    print(f"  2. Or run directly:")
    print(f"     {C['c']}{cmd}{C['x']}")
    print("")

    start_now = ask("Start worker now? [Y/n]", "Y").lower()
    if start_now in ("y", "yes", ""):
        print("")
        info("Starting worker...")
        print(f"{C['dim']}─" * 55 + f"{C['x']}\n")
        try:
            os.chdir(INSTALL_DIR)
            if IS_WIN:
                # Build command as list to avoid shell parsing issues
                cmd_list = [sys.executable, str(WORKER_PATH), "--pool", pool_url, "--name", worker_name]
                if wallet:
                    cmd_list.extend(["--wallet", wallet])
                if use_cpu:
                    cmd_list.append("--cpu")
                subprocess.run(cmd_list)
            else:
                subprocess.run(cmd, shell=True)
        except KeyboardInterrupt:
            pass
        except Exception as e:
            err(f"Failed to start worker: {e}")
            print(f"  Try running manually: {cmd}")
    else:
        ok("Done. Run the desktop shortcut anytime to start.")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{C['y']}[!] Cancelled by user.{C['x']}")
        sys.exit(0)
    except Exception as e:
        import traceback
        print(f"\n{C['r']}[✗] Installer crashed: {e}{C['x']}")
        print(f"{C['dim']}--- Traceback ---{C['x']}")
        traceback.print_exc()
        print(f"{C['dim']}-----------------{C['x']}")
        print(f"\n{C['y']}[!] Please screenshot this error and report it.{C['x']}")
        sys.exit(1)
