"""Puzzle #135 (Kangaroo) pool endpoints."""

import asyncio
import logging
import random
import time

from fastapi import APIRouter
from pydantic import BaseModel

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/api/135")

# Puzzle #135 parameters
P135_PUBKEY = "02145d2611c823a396ef6712ce0f712f09b9b4f3135e3e0aa3230fb9b6d08d1e16"
P135_RANGE_START = 2**134
P135_RANGE_END = 2**135 - 1
P135_TOTAL_BITS = 134
P135_CHUNK_BITS = 48  # Each chunk is 2^48 keys (~280 trillion)
P135_CHUNK_SIZE = 2**48

# In-memory chunk tracking
_assigned_chunks: dict[int, dict] = {}
_completed_chunks: set[int] = set()
_chunk_lock = False

# v1.1: per-worker wallet + contribution tracking (for manual payouts)
_worker_registry: dict[str, dict] = {}  # worker_name -> {wallet, chunks_completed, keys_scanned, last_seen}


class P135WorkRequest(BaseModel):
    worker_name: str
    wallet_address: str | None = None  # BTC payout address


class P135CompleteRequest(BaseModel):
    chunk_id: int
    worker_name: str
    keys_found: int = 0
    private_key: str | None = None
    ops_performed: int = 0


@router.post("/work")
async def get_p135_work(body: P135WorkRequest):
    """Assign a random unsearched chunk of Puzzle #135."""
    global _chunk_lock
    while _chunk_lock:
        await asyncio.sleep(0.01)
    _chunk_lock = True
    try:
        # Register/update worker wallet
        if body.worker_name not in _worker_registry:
            _worker_registry[body.worker_name] = {
                "wallet": body.wallet_address or "",
                "chunks_completed": 0,
                "keys_scanned": 0,
                "last_seen": time.time(),
            }
        else:
            if body.wallet_address:
                _worker_registry[body.worker_name]["wallet"] = body.wallet_address
            _worker_registry[body.worker_name]["last_seen"] = time.time()

        for _ in range(1000):
            chunk_id = random.randint(0, (2**38) - 1)
            if chunk_id in _assigned_chunks or chunk_id in _completed_chunks:
                continue

            range_start = P135_RANGE_START + (chunk_id * P135_CHUNK_SIZE)
            range_end = range_start + P135_CHUNK_SIZE - 1

            _assigned_chunks[chunk_id] = {
                "worker_name": body.worker_name,
                "assigned_at": time.time(),
                "status": "active",
            }

            return {
                "status": "ok",
                "pubkey": P135_PUBKEY,
                "range_start": hex(range_start),
                "range_end": hex(range_end),
                "chunk_id": chunk_id,
                "dp_bits": 28,
                "checkpoint_interval": 300,
            }

        return {"status": "no_work", "message": "All chunks currently assigned"}
    finally:
        _chunk_lock = False


@router.post("/complete")
async def complete_p135_work(body: P135CompleteRequest):
    """Report chunk completion."""
    global _chunk_lock
    while _chunk_lock:
        await asyncio.sleep(0.01)
    _chunk_lock = True
    try:
        if body.chunk_id in _assigned_chunks:
            del _assigned_chunks[body.chunk_id]
        _completed_chunks.add(body.chunk_id)

        # Update worker stats
        w = _worker_registry.get(body.worker_name)
        if w:
            w["chunks_completed"] = w.get("chunks_completed", 0) + 1
            w["keys_scanned"] = w.get("keys_scanned", 0) + P135_CHUNK_SIZE
            w["last_seen"] = time.time()

        if body.keys_found > 0 and body.private_key:
            logger.critical(
                "PUZZLE 135 KEY FOUND by %s! Key: %s",
                body.worker_name, body.private_key
            )
            with open("FOUND_KEY_135.txt", "w") as f:
                f.write(f"PUZZLE #135 SOLVED!\n")
                f.write(f"Private Key: {body.private_key}\n")
                f.write(f"Found by: {body.worker_name}\n")
                f.write(f"Time: {time.time()}\n")

        return {"status": "ok", "accepted": True}
    finally:
        _chunk_lock = False


@router.get("/stats")
async def p135_stats():
    """Public stats for Puzzle #135 pool."""
    total_assigned = len(_assigned_chunks)
    total_completed = len(_completed_chunks)
    keys_scanned = total_completed * P135_CHUNK_SIZE
    total_keyspace = P135_RANGE_END - P135_RANGE_START + 1
    progress_pct = (keys_scanned / total_keyspace) * 100 if total_keyspace > 0 else 0

    # Build worker leaderboard
    workers = []
    for name, info in sorted(
        _worker_registry.items(),
        key=lambda x: x[1].get("keys_scanned", 0),
        reverse=True
    ):
        workers.append({
            "name": name,
            "wallet": info.get("wallet", ""),
            "chunks_completed": info.get("chunks_completed", 0),
            "keys_scanned": info.get("keys_scanned", 0),
            "last_seen": info.get("last_seen", 0),
        })

    return {
        "puzzle": {
            "number": 135,
            "pubkey": P135_PUBKEY,
            "range_start": hex(P135_RANGE_START),
            "range_end": hex(P135_RANGE_END),
            "total_bits": P135_TOTAL_BITS,
            "chunk_size_bits": P135_CHUNK_BITS,
        },
        "progress": {
            "chunks_assigned": total_assigned,
            "chunks_completed": total_completed,
            "keys_scanned": keys_scanned,
            "total_keyspace": total_keyspace,
            "percentage": progress_pct,
        },
        "pool": {
            "active_workers": len(set(c["worker_name"] for c in _assigned_chunks.values())),
            "total_workers": len(_worker_registry),
            "workers": workers,
        },
    }


@router.get("/payouts")
async def p135_payouts(reward_btc: float = 13.5):
    """Calculate proportional manual payouts for admin review.

    Payouts are NOT automatic — the pool admin uses this to see
    who contributed what, then sends BTC manually.
    """
    total_keys = sum(
        info.get("keys_scanned", 0) for info in _worker_registry.values()
    )

    if total_keys == 0:
        return {
            "reward_btc": reward_btc,
            "total_keys": 0,
            "payouts": [],
            "note": "No work completed yet. Payouts are manual — this is a reference for the admin.",
        }

    payouts = []
    for name, info in sorted(
        _worker_registry.items(),
        key=lambda x: x[1].get("keys_scanned", 0),
        reverse=True
    ):
        keys = info.get("keys_scanned", 0)
        share = keys / total_keys
        payouts.append({
            "worker": name,
            "wallet": info.get("wallet", ""),
            "chunks_completed": info.get("chunks_completed", 0),
            "keys_scanned": keys,
            "share_percent": round(share * 100, 6),
            "payout_btc": round(share * reward_btc, 8),
        })

    return {
        "reward_btc": reward_btc,
        "total_keys": total_keys,
        "payouts": payouts,
        "note": "Payouts are MANUAL. Admin sends BTC to each wallet based on this breakdown.",
    }
