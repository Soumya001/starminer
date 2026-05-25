"""Puzzle #135 Pool Server — FastAPI coordinator for Starminer workers."""

import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware

from . import database as db
from .routes import pool_v1, puzzle135

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)

DB_PATH = "puzzle135.db"


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Initializing database at %s", DB_PATH)
    await db.init_db(DB_PATH)
    logger.info("Puzzle #135 pool server ready")
    yield
    await db.close_db()
    logger.info("Database closed")


app = FastAPI(
    title="Puzzle #135 Pool",
    description="Coordinated Kangaroo search for Bitcoin Puzzle #135",
    version="2.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Primary starminer protocol
app.include_router(pool_v1.router)

# Legacy endpoints (old puzzle135.py prefix /api/135/) — kept so any old
# clients don't hard-404, but they no longer have a live backend behind them.
# The new server is the source of truth.
app.include_router(puzzle135.router)

STATIC_DIR = Path(__file__).parent / "static"
BINARY_DIR = Path(__file__).parent.parent / "binaries"
INSTALLER_DIR = Path(__file__).parent.parent / "installer"


@app.get("/")
async def root():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/puzzle135")
async def puzzle_135_page():
    return FileResponse(STATIC_DIR / "index.html")


# ---------------------------------------------------------------------------
# Legacy /api/135/stats → forward to new stats so the existing frontend works
# ---------------------------------------------------------------------------

@app.get("/api/135/stats")
async def legacy_135_stats():
    from .routes.pool_v1 import pool_stats
    return await pool_stats()


# ---------------------------------------------------------------------------
# Installer scripts
# ---------------------------------------------------------------------------

@app.get("/install-135.sh")
async def install_script_sh():
    path = INSTALLER_DIR / "install.sh"
    if path.exists():
        return FileResponse(path, media_type="text/x-shellscript")
    return {"error": "Installer script not found"}


@app.get("/install-135.ps1")
async def install_script_ps1():
    path = INSTALLER_DIR / "install.ps1"
    if path.exists():
        return FileResponse(path, media_type="text/plain")
    return {"error": "Installer script not found"}


# ---------------------------------------------------------------------------
# Binary downloads
# ---------------------------------------------------------------------------

@app.get("/download/starminer-linux-x64-cuda")
async def dl_starminer_linux_cuda():
    path = BINARY_DIR / "starminer-linux"
    if path.exists():
        return FileResponse(path, filename="starminer", media_type="application/octet-stream")
    return {"error": "Linux CUDA binary not available yet — use GitHub releases"}


@app.get("/download/starminer-linux-x64-cpu")
async def dl_starminer_linux_cpu():
    path = BINARY_DIR / "starminer-linux-x64-cpu"
    if path.exists():
        return FileResponse(path, filename="starminer", media_type="application/octet-stream")
    return {"error": "Linux CPU binary not available yet — use GitHub releases"}


@app.get("/download/starminer-windows-x64-cuda.exe")
async def dl_starminer_windows_cuda():
    path = BINARY_DIR / "starminer-windows.exe"
    if path.exists():
        return FileResponse(path, filename="starminer.exe", media_type="application/octet-stream")
    return {"error": "Windows CUDA binary not available yet — use GitHub releases"}


@app.get("/download/starminer-windows-x64-cpu.exe")
async def dl_starminer_windows_cpu():
    path = BINARY_DIR / "starminer-windows-x64-cpu.exe"
    if path.exists():
        return FileResponse(path, filename="starminer.exe", media_type="application/octet-stream")
    return {"error": "Windows CPU binary not available yet — use GitHub releases"}


@app.get("/download/starminer-macos-arm64")
async def dl_starminer_macos():
    path = BINARY_DIR / "starminer-macos"
    if path.exists():
        return FileResponse(path, filename="starminer", media_type="application/octet-stream")
    return {"error": "macOS binary not available yet — use GitHub releases"}


@app.get("/download/puzzle135_worker.py")
async def dl_worker_py():
    path = Path(__file__).parent.parent / "worker" / "puzzle135_worker.py"
    if path.exists():
        return FileResponse(path, filename="puzzle135_worker.py")
    return {"error": "Worker script not found"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("server.main:app", host="0.0.0.0", port=8421, reload=False)
