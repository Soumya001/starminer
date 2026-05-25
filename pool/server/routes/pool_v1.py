"""Starminer HTTP pool protocol — /api/v1/pool/ endpoints for Puzzle #135."""

import logging
import secrets
import time

from fastapi import APIRouter, Query
from pydantic import BaseModel

from .. import database as db

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/api/v1/pool")

# ---------------------------------------------------------------------------
# Puzzle constants
# ---------------------------------------------------------------------------
P135_PUBKEY     = "02145d2611c823a396ef6712ce0f712f09b9b4f3135e3e0aa3230fb9b6d08d1e16"
P135_RANGE_START = 2**134
P135_RANGE_END   = 2**135 - 1
P135_DP_BITS     = 28          # leading zero bits required for a DP
P135_WORK_TIMEOUT = 3600       # seconds before work expires without a ping
P135_PING_TIMEOUT = 300        # seconds between pings before work is reclaimed

_server_start = time.time()

# ---------------------------------------------------------------------------
# Live speed tracking (DPs/s over last 60 s)
# ---------------------------------------------------------------------------
_dp_timestamps: list[float] = []
_MAX_DP_WINDOW = 60.0


def _record_dp_event(count: int):
    now = time.time()
    _dp_timestamps.extend([now] * count)
    # Trim old events
    cutoff = now - _MAX_DP_WINDOW
    while _dp_timestamps and _dp_timestamps[0] < cutoff:
        _dp_timestamps.pop(0)


def _live_dps() -> float:
    now = time.time()
    cutoff = now - _MAX_DP_WINDOW
    recent = sum(1 for t in _dp_timestamps if t >= cutoff)
    return recent / _MAX_DP_WINDOW


# ---------------------------------------------------------------------------
# In-memory solution flag (also persisted to DB)
# ---------------------------------------------------------------------------
_solution_found: bool = False
_solution_key: str = ""


# ---------------------------------------------------------------------------
# Request/response models
# ---------------------------------------------------------------------------

class AuthRequest(BaseModel):
    worker_name: str
    version: str = ""
    pool_type: str = "http"
    client: str = ""
    password: str = ""
    wallet: str = ""


class WorkRequest(BaseModel):
    worker_name: str
    session_token: str = ""


class DPItem(BaseModel):
    x: str
    d: str
    type: int       # 0 = tame, 1 = wild
    sequence: int = 0
    dp_bits: int = P135_DP_BITS


class Telemetry(BaseModel):
    pass
    model_config = {"extra": "allow"}


class DPBatchRequest(BaseModel):
    worker_name: str
    session_token: str = ""
    work_id: int = 0
    dps: list[DPItem] = []
    telemetry: dict = {}


class PingRequest(BaseModel):
    worker_name: str
    session_token: str = ""


class SolutionRequest(BaseModel):
    worker_name: str
    session_token: str = ""
    private_key: str
    work_id: int = 0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _range_hex(val: int) -> str:
    """Return 32-byte big-endian hex string (64 chars) for a large int."""
    return val.to_bytes(32, "big").hex()


async def _resolve_session(token: str, name: str) -> dict | None:
    """Return session dict if valid, else None."""
    if not token:
        return None
    sess = await db.get_session(token)
    if sess and sess["name"] == name:
        return sess
    return None


# ---------------------------------------------------------------------------
# POST /auth
# ---------------------------------------------------------------------------

@router.post("/auth")
async def pool_auth(body: AuthRequest):
    """Worker authentication — returns session_token."""
    if not body.worker_name or len(body.worker_name) > 64:
        return {"auth_ok": False, "error": "Invalid worker_name"}

    wallet = body.wallet.strip()
    worker_id = await db.upsert_worker(body.worker_name, wallet, body.version)

    token = secrets.token_hex(32)
    await db.create_session(worker_id, token)

    logger.info("Worker '%s' authenticated (id=%d, version=%s)", body.worker_name, worker_id, body.version)
    return {"auth_ok": True, "session_token": token}


# ---------------------------------------------------------------------------
# POST /work
# ---------------------------------------------------------------------------

@router.post("/work")
async def pool_work(body: WorkRequest):
    """Assign a Kangaroo search assignment."""
    global _solution_found

    if _solution_found:
        return {"error": "Solution already found — pool is closed"}

    sess = await _resolve_session(body.session_token, body.worker_name)
    if sess is None:
        return {"error": "Invalid or missing session_token — please re-authenticate"}

    # Expire stale active work before assigning new
    expired = await db.expire_stale_work(time.time())
    if expired:
        logger.info("Expired %d stale work assignments", expired)

    deadline = time.time() + P135_WORK_TIMEOUT
    work_id = await db.create_work(
        sess["worker_id"],
        _range_hex(P135_RANGE_START),
        _range_hex(P135_RANGE_END),
        P135_DP_BITS,
        deadline,
    )
    await db.increment_worker_stats(sess["worker_id"], dp_count=0, work_count=1)

    logger.info("Assigned work_id=%d to worker '%s'", work_id, body.worker_name)
    return {
        "public_key":  P135_PUBKEY,
        "range_start": _range_hex(P135_RANGE_START),
        "range_end":   _range_hex(P135_RANGE_END),
        "dp_bits":     P135_DP_BITS,
        "work_id":     work_id,
        "puzzle_name": "Puzzle #135",
    }


# ---------------------------------------------------------------------------
# POST /dp_batch
# ---------------------------------------------------------------------------

@router.post("/dp_batch")
async def pool_dp_batch(body: DPBatchRequest):
    """Receive a batch of distinguished points, check for collision."""
    global _solution_found, _solution_key

    if _solution_found:
        return {"solution_found": True, "private_key": _solution_key}

    sess = await _resolve_session(body.session_token, body.worker_name)
    if sess is None:
        return {"error": "Invalid session_token"}

    if not body.dps:
        return {"solution_found": False}

    # Validate types
    for dp in body.dps:
        if dp.type not in (0, 1):
            return {"error": f"Invalid DP type {dp.type} — must be 0 (tame) or 1 (wild)"}

    dp_dicts = [{"x": dp.x, "d": dp.d, "type": dp.type,
                 "sequence": dp.sequence, "dp_bits": dp.dp_bits}
                for dp in body.dps]

    # Check for collision BEFORE saving (fast check against existing x values)
    collision = None
    for dp in body.dps:
        col = await db.find_collision(dp.x)
        if col:
            a, b = col["a"], col["b"]
            if a["type"] != b["type"]:
                collision = col
                logger.critical(
                    "COLLISION DETECTED! work_id=%d x=%s tame_d=%s wild_d=%s",
                    body.work_id, dp.x,
                    (a["d"] if a["type"] == 0 else b["d"]),
                    (a["d"] if a["type"] == 1 else b["d"]),
                )
                break

    # Save all DPs
    saved = await db.save_dps(body.work_id, sess["worker_id"], dp_dicts)
    await db.increment_worker_stats(sess["worker_id"], dp_count=saved)
    _record_dp_event(saved)

    # Ping the work assignment to keep it alive
    if body.work_id:
        await db.ping_work(body.work_id, time.time() + P135_WORK_TIMEOUT)

    if collision:
        _solution_found = True
        _solution_key = "COLLISION_DETECTED_MANUAL_RECOVERY_NEEDED"
        await db.record_solution(_solution_key, sess["worker_id"], body.work_id)
        logger.critical(
            "Puzzle #135 COLLISION: tame_d=%s wild_d=%s — admin must compute private key",
            collision["a"]["d"], collision["b"]["d"],
        )
        return {"solution_found": True, "private_key": ""}

    logger.debug("Worker '%s' submitted %d DPs for work_id=%d", body.worker_name, saved, body.work_id)
    return {"solution_found": False}


# ---------------------------------------------------------------------------
# POST /ping
# ---------------------------------------------------------------------------

@router.post("/ping")
async def pool_ping(body: PingRequest):
    """Heartbeat — keep work assignment alive."""
    sess = await _resolve_session(body.session_token, body.worker_name)
    if sess is None:
        return {"ok": False}

    await db.touch_session(body.session_token)

    # Extend all active assignments for this worker
    active = await db.get_active_work_ids()
    # (we don't store work_id in ping, so just touch session — work expires on its own TTL)

    return {"ok": True, "solution_found": _solution_found}


# ---------------------------------------------------------------------------
# GET /stats
# ---------------------------------------------------------------------------

_stats_cache: dict | None = None
_stats_cache_time: float = 0.0
_STATS_TTL = 5.0


@router.get("/stats")
async def pool_stats(
    worker: str = Query(default=""),
    token: str = Query(default=""),
):
    """Pool statistics (cached 5 s)."""
    global _stats_cache, _stats_cache_time

    now = time.time()
    if _stats_cache is not None and (now - _stats_cache_time) < _STATS_TTL:
        result = dict(_stats_cache)
        if worker:
            result["your_dps"] = await _worker_dp_count(worker)
            result["your_share"] = result["your_dps"] / max(result["total_dps"], 1)
        return result

    raw = await db.get_pool_stats()
    leaderboard = await db.get_leaderboard(limit=50)

    result = {
        "total_dps":       raw["total_dps"],
        "total_workers":   raw["total_workers"],
        "active_workers":  raw["active_workers"],
        "active_assignments": raw["active_assignments"],
        "completed_work":  raw["completed_work"],
        "dps_per_second":  round(_live_dps(), 2),
        "uptime_seconds":  round(now - _server_start),
        "solution_found":  raw["solution_found"] or _solution_found,
        "your_share":      0.0,
        "your_dps":        0,
        "leaderboard":     leaderboard,
        # Fields the frontend expects
        "puzzle": {
            "number":      135,
            "pubkey":      P135_PUBKEY,
            "range_start": hex(P135_RANGE_START),
            "range_end":   hex(P135_RANGE_END),
            "dp_bits":     P135_DP_BITS,
        },
        "pool": {
            "total_workers":  raw["total_workers"],
            "active_workers": raw["active_workers"],
            "total_dps":      raw["total_dps"],
            "dps_per_second": round(_live_dps(), 2),
            "solution_found": raw["solution_found"] or _solution_found,
            "uptime_seconds": round(now - _server_start),
            "workers":        leaderboard,
        },
        "progress": {
            "total_dps":   raw["total_dps"],
            "completed_work": raw["completed_work"],
        },
    }

    _stats_cache = result
    _stats_cache_time = now

    if worker:
        result = dict(result)
        result["your_dps"] = await _worker_dp_count(worker)
        result["your_share"] = result["your_dps"] / max(raw["total_dps"], 1)

    return result


async def _worker_dp_count(name: str) -> int:
    lb = await db.get_leaderboard(limit=10000)
    for w in lb:
        if w["name"] == name:
            return w["total_dps"]
    return 0


# ---------------------------------------------------------------------------
# POST /solution
# ---------------------------------------------------------------------------

@router.post("/solution")
async def pool_solution(body: SolutionRequest):
    """Explicit private key submission from a worker."""
    global _solution_found, _solution_key

    sess = await _resolve_session(body.session_token, body.worker_name)
    if sess is None:
        return {"accepted": False, "error": "Invalid session_token"}

    if not body.private_key or len(body.private_key) > 128:
        return {"accepted": False, "error": "Invalid private_key"}

    _solution_found = True
    _solution_key = body.private_key

    await db.record_solution(body.private_key, sess["worker_id"], body.work_id or None)

    logger.critical(
        "PUZZLE #135 SOLUTION SUBMITTED by '%s': %s",
        body.worker_name, body.private_key,
    )
    return {"accepted": True}

