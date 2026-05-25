"""JLP Pool Server — asyncio TCP server implementing the JLP Kangaroo protocol.

Listens on port 17403 (default). Workers connect with:
  --pool jlp://host:17403          (plaintext)
  --pool jlps://host:17403         (TLS — recommended for internet / Vast.ai)

Shares data/pool_dps.db with the FastAPI HTTP server (pool_v1.py) so stats
are visible on both the web dashboard and via STATS_RSP to binary workers.

Vast.ai workers: run starminer with --pool jlp://<public_ip>:17403 or use
  the docker template from pool/jlp_server/vastai_template.json.
"""

import argparse
import asyncio
import collections
import logging
import os
import random
import ssl
import struct
import time
from pathlib import Path

import aiosqlite

from protocol import (
    HEADER_SIZE, MAX_PAYLOAD,
    AUTH, AUTH_OK, AUTH_FAIL, WORK_REQ, WORK_ASN,
    DP_SUBMIT, DP_ACK, DP_BATCH, DP_BATCH_V2, DP_SUBMIT_V2,
    STATS_REQ, STATS_RSP, SOLUTION, PING, PONG, MSG_ERROR,
    pack_header, parse_header,
    pack_auth_ok, pack_auth_fail, pack_pong, pack_dp_ack,
    pack_work_assignment, pack_stats_rsp, pack_solution_notify,
    parse_auth_payload, parse_dp_batch_v2, parse_dp_v1, parse_solution,
)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S',
)
logger = logging.getLogger(__name__)

# ── Config ────────────────────────────────────────────────────────────────────
PUBKEY        = "02145d2611c823a396ef6712ce0f712f09b9b4f3135e3e0aa3230fb9b6d08d1e16"
RANGE_START   = 2**134
RANGE_END     = 2**135 - 1
CHUNK_BITS    = 48
CHUNK_SIZE    = 2**CHUNK_BITS
CHUNK_TIMEOUT = 1800   # seconds before a work assignment expires
DP_BITS       = 28

_HERE      = Path(__file__).resolve().parent
DB_PATH    = _HERE.parent.parent / "data" / "pool_dps.db"
FOUND_PATH = DB_PATH.parent / "FOUND_KEY_135.txt"

_server_start = time.time()
_dp_times: collections.deque = collections.deque()
_DP_WINDOW = 60.0


def _live_dps_per_sec() -> float:
    now = time.time()
    cutoff = now - _DP_WINDOW
    while _dp_times and _dp_times[0] < cutoff:
        _dp_times.popleft()
    return len(_dp_times) / _DP_WINDOW


# ── DB helpers ────────────────────────────────────────────────────────────────
async def ensure_db():
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    async with aiosqlite.connect(str(DB_PATH)) as db:
        await db.execute("PRAGMA journal_mode=WAL")
        await db.execute("PRAGMA synchronous=NORMAL")
        await db.executescript("""
            CREATE TABLE IF NOT EXISTS workers (
                worker_name TEXT PRIMARY KEY,
                total_dps   INTEGER DEFAULT 0,
                first_seen  REAL DEFAULT (unixepoch()),
                last_seen   REAL DEFAULT (unixepoch())
            );
            CREATE TABLE IF NOT EXISTS sessions (
                token       TEXT PRIMARY KEY,
                worker_name TEXT NOT NULL,
                created_at  REAL DEFAULT (unixepoch()),
                expires_at  REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_sess_worker ON sessions(worker_name);
            CREATE TABLE IF NOT EXISTS assignments (
                work_id     INTEGER PRIMARY KEY AUTOINCREMENT,
                worker_name TEXT NOT NULL,
                range_start TEXT NOT NULL,
                range_end   TEXT NOT NULL,
                dp_bits     INTEGER DEFAULT 28,
                assigned_at REAL NOT NULL,
                status      TEXT DEFAULT 'active'
            );
            CREATE INDEX IF NOT EXISTS idx_asgn_status ON assignments(status);
            CREATE INDEX IF NOT EXISTS idx_asgn_range  ON assignments(range_start);
            CREATE TABLE IF NOT EXISTS dps (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                work_id     INTEGER NOT NULL,
                worker_name TEXT NOT NULL,
                x           TEXT NOT NULL COLLATE NOCASE,
                d           TEXT NOT NULL COLLATE NOCASE,
                type        INTEGER NOT NULL,
                dp_bits     INTEGER NOT NULL,
                sequence    INTEGER,
                created_at  REAL DEFAULT (unixepoch())
            );
            CREATE INDEX IF NOT EXISTS idx_dps_x    ON dps(x);
            CREATE INDEX IF NOT EXISTS idx_dps_type ON dps(type);
            CREATE TABLE IF NOT EXISTS solutions (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                private_key TEXT NOT NULL,
                found_by    TEXT NOT NULL,
                work_id     INTEGER,
                created_at  REAL DEFAULT (unixepoch())
            );
            CREATE TABLE IF NOT EXISTS shares (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                worker_name TEXT NOT NULL,
                period_hour TEXT NOT NULL,
                dps_count   INTEGER DEFAULT 0,
                mkeys_avg   REAL DEFAULT 0.0,
                created_at  REAL DEFAULT (unixepoch()),
                UNIQUE(worker_name, period_hour)
            );
        """)
        await db.commit()


async def upsert_worker(db: aiosqlite.Connection, worker_name: str):
    await db.execute(
        "INSERT INTO workers (worker_name) VALUES (?) "
        "ON CONFLICT(worker_name) DO UPDATE SET last_seen = unixepoch()",
        (worker_name,),
    )
    await db.commit()


async def check_solution(db: aiosqlite.Connection) -> dict | None:
    async with db.execute(
        "SELECT private_key, found_by FROM solutions ORDER BY id DESC LIMIT 1"
    ) as cur:
        row = await cur.fetchone()
    return dict(row) if row else None


async def assign_work(db: aiosqlite.Connection, worker_name: str) -> dict:
    cutoff = time.time() - CHUNK_TIMEOUT
    await db.execute(
        "UPDATE assignments SET status='expired' WHERE status='active' AND assigned_at < ?",
        (cutoff,),
    )
    await db.commit()

    for _ in range(300):
        chunk_id = random.randint(0, (2**38) - 1)
        rs = RANGE_START + chunk_id * CHUNK_SIZE
        re = rs + CHUNK_SIZE - 1
        rs_hex = f"0x{rs:064x}"
        re_hex = f"0x{re:064x}"

        async with db.execute(
            "SELECT 1 FROM assignments WHERE range_start=? AND status IN ('active','completed') LIMIT 1",
            (rs_hex,),
        ) as cur:
            if await cur.fetchone():
                continue

        async with db.execute(
            "INSERT INTO assignments (worker_name, range_start, range_end, dp_bits, assigned_at)"
            " VALUES (?,?,?,?,?)",
            (worker_name, rs_hex, re_hex, DP_BITS, time.time()),
        ) as cur:
            work_id = cur.lastrowid
        await db.commit()
        return {"work_id": work_id, "range_start": rs, "range_end": re}

    raise RuntimeError("No free chunks — try again shortly")


async def save_dps(db: aiosqlite.Connection, worker_name: str, dps: list[dict]) -> int:
    for dp in dps:
        await db.execute(
            "INSERT INTO dps (work_id, worker_name, x, d, type, dp_bits, sequence)"
            " VALUES (?,?,?,?,?,?,?)",
            (dp.get('work_id', 0), worker_name,
             dp['x'], dp['d'], dp['type'], dp['dp_bits'], dp.get('sequence')),
        )
    await db.execute(
        "UPDATE workers SET total_dps = total_dps + ?, last_seen = unixepoch()"
        " WHERE worker_name = ?",
        (len(dps), worker_name),
    )
    await db.commit()
    return len(dps)


async def check_collision(db: aiosqlite.Connection, dps: list[dict]) -> str | None:
    """Check if any submitted DP collides with an existing DP of opposite type."""
    for dp in dps:
        opposite = 1 - dp['type']
        async with db.execute(
            "SELECT x, d, type, work_id FROM dps WHERE x=? AND type=? LIMIT 1",
            (dp['x'], opposite),
        ) as cur:
            hit = await cur.fetchone()
        if hit:
            logger.critical("COLLISION DETECTED! x=%s type=%d vs type=%d",
                            dp['x'], dp['type'], opposite)
            return dp['x']
    return None


async def get_stats(db: aiosqlite.Connection, worker_name: str = "") -> dict:
    async with db.execute("SELECT COUNT(*) FROM dps") as c:
        total_dps = (await c.fetchone())[0]
    async with db.execute("SELECT COUNT(*) FROM workers") as c:
        total_workers = (await c.fetchone())[0]
    async with db.execute(
        "SELECT COUNT(*) FROM workers WHERE last_seen > unixepoch() - 300"
    ) as c:
        active_workers = (await c.fetchone())[0]

    your_dps = 0
    if worker_name:
        async with db.execute(
            "SELECT total_dps FROM workers WHERE worker_name=?", (worker_name,)
        ) as c:
            row = await c.fetchone()
        your_dps = row[0] if row else 0

    your_share = (your_dps / total_dps) if total_dps > 0 else 0.0
    return {
        'total_dps': total_dps,
        'total_workers': total_workers,
        'active_workers': active_workers,
        'your_dps': your_dps,
        'your_share': your_share,
    }


# ── Per-connection state ──────────────────────────────────────────────────────
class WorkerSession:
    def __init__(self, peer: str):
        self.peer = peer
        self.worker_name: str = ""
        self.authenticated: bool = False
        self.current_work_id: int | None = None
        self.last_seen: float = time.time()


# ── Frame I/O ─────────────────────────────────────────────────────────────────
async def read_frame(reader: asyncio.StreamReader) -> tuple[int, bytes]:
    """Read one complete JLP frame. Returns (msg_type, payload_bytes)."""
    hdr_bytes = await asyncio.wait_for(reader.readexactly(HEADER_SIZE), timeout=90)
    msg_type, flags, payload_size = parse_header(hdr_bytes)
    if payload_size > MAX_PAYLOAD:
        raise ValueError(f"Payload {payload_size} > max {MAX_PAYLOAD}")
    payload = b''
    if payload_size > 0:
        payload = await asyncio.wait_for(reader.readexactly(payload_size), timeout=30)
    return msg_type, payload


async def send_frame(writer: asyncio.StreamWriter, data: bytes):
    writer.write(data)
    await writer.drain()


# ── Connection handler ────────────────────────────────────────────────────────
async def handle_connection(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info('peername', ('?', 0))
    peer_str = f"{peer[0]}:{peer[1]}"
    session = WorkerSession(peer_str)
    logger.info("New connection from %s", peer_str)

    # AUTH must arrive within 30 seconds
    try:
        msg_type, payload = await asyncio.wait_for(read_frame(reader), timeout=30)
    except asyncio.TimeoutError:
        logger.warning("Auth timeout from %s", peer_str)
        writer.close()
        return
    except Exception as e:
        logger.debug("Auth read error from %s: %s", peer_str, e)
        writer.close()
        return

    if msg_type != AUTH:
        await send_frame(writer, pack_auth_fail("Must send AUTH first"))
        writer.close()
        return

    try:
        worker_name, password = parse_auth_payload(payload)
    except ValueError as e:
        await send_frame(writer, pack_auth_fail(str(e)))
        writer.close()
        return

    if not worker_name:
        await send_frame(writer, pack_auth_fail("Empty worker name"))
        writer.close()
        return

    session.worker_name = worker_name
    session.authenticated = True

    async with aiosqlite.connect(str(DB_PATH)) as db:
        db.row_factory = aiosqlite.Row
        await db.execute("PRAGMA journal_mode=WAL")
        await db.execute("PRAGMA synchronous=NORMAL")
        await upsert_worker(db, worker_name)

    await send_frame(writer, pack_auth_ok())
    logger.info("Authenticated: %s  peer=%s", worker_name, peer_str)

    # Main message loop
    try:
        async with aiosqlite.connect(str(DB_PATH)) as db:
            db.row_factory = aiosqlite.Row
            await db.execute("PRAGMA journal_mode=WAL")
            await db.execute("PRAGMA synchronous=NORMAL")

            while True:
                try:
                    msg_type, payload = await asyncio.wait_for(
                        read_frame(reader), timeout=90
                    )
                except asyncio.TimeoutError:
                    logger.info("Keepalive timeout, dropping %s (%s)", worker_name, peer_str)
                    break

                session.last_seen = time.time()

                # ── Handle each message type ──────────────────────────────────

                if msg_type == PING:
                    await send_frame(writer, pack_pong())

                elif msg_type == WORK_REQ:
                    sol = await check_solution(db)
                    if sol:
                        await send_frame(writer, pack_solution_notify(sol['private_key']))
                        logger.info("Sent SOLUTION to %s (puzzle already solved)", worker_name)
                        break

                    try:
                        work = await assign_work(db, worker_name)
                    except RuntimeError as e:
                        err_payload = str(e).encode()[:255]
                        await send_frame(writer,
                            pack_header(MSG_ERROR, len(err_payload)) + err_payload)
                        continue

                    session.current_work_id = work['work_id']
                    frame = pack_work_assignment(
                        PUBKEY, work['range_start'], work['range_end'],
                        DP_BITS, work['work_id'],
                    )
                    await send_frame(writer, frame)
                    logger.info("Work → %s  id=%d  0x%x..0x%x",
                                worker_name, work['work_id'],
                                work['range_start'], work['range_end'])

                elif msg_type in (DP_BATCH_V2, DP_BATCH):
                    # DP_BATCH_V2: [count:u32 LE][dp1:78]...
                    # DP_BATCH (v1): [count:u32 LE][dp1:66]... — legacy, treat same
                    if msg_type == DP_BATCH_V2:
                        try:
                            dps = parse_dp_batch_v2(payload)
                        except ValueError as e:
                            logger.warning("Bad DP_BATCH_V2 from %s: %s", worker_name, e)
                            await send_frame(writer, pack_dp_ack())
                            continue
                    else:
                        # v1 batch: same count prefix but 66-byte entries without work_id/seq
                        try:
                            count = struct.unpack_from('<I', payload, 0)[0]
                            dps = []
                            for i in range(min(count, 10000)):
                                off = 4 + i * 66
                                if off + 66 > len(payload):
                                    break
                                dp = parse_dp_v1(payload[off:off + 66])
                                dp['work_id'] = session.current_work_id or 0
                                dp['sequence'] = None
                                dps.append(dp)
                        except Exception as e:
                            logger.warning("Bad DP_BATCH from %s: %s", worker_name, e)
                            await send_frame(writer, pack_dp_ack())
                            continue

                    if not dps:
                        await send_frame(writer, pack_dp_ack())
                        continue

                    sol = await check_solution(db)
                    if sol:
                        await send_frame(writer, pack_solution_notify(sol['private_key']))
                        break

                    accepted = await save_dps(db, worker_name, dps)
                    if accepted:
                        now_t = time.time()
                        _dp_times.extend([now_t] * accepted)

                    # Check for Kangaroo collision
                    collision_x = await check_collision(db, dps)
                    if collision_x:
                        logger.critical("Potential collision on x=%s from %s", collision_x, worker_name)

                    await send_frame(writer, pack_dp_ack())

                elif msg_type in (DP_SUBMIT_V2, DP_SUBMIT):
                    # Single DP submission
                    try:
                        if msg_type == DP_SUBMIT_V2:
                            from protocol import _DP_V2
                            work_id, seq, x_b, d_b, dp_type, dp_bits = _DP_V2.unpack_from(payload)
                            dp = {'work_id': work_id, 'sequence': seq,
                                  'x': x_b.hex(), 'd': d_b.hex(),
                                  'type': dp_type, 'dp_bits': dp_bits}
                        else:
                            dp = parse_dp_v1(payload)
                            dp['work_id'] = session.current_work_id or 0
                            dp['sequence'] = None

                        await save_dps(db, worker_name, [dp])
                        now_t = time.time()
                        _dp_times.append(now_t)
                    except Exception as e:
                        logger.warning("Bad DP_SUBMIT from %s: %s", worker_name, e)
                    await send_frame(writer, pack_dp_ack())

                elif msg_type == STATS_REQ:
                    stats = await get_stats(db, worker_name)
                    sol = await check_solution(db)
                    frame = pack_stats_rsp(
                        total_dps     = stats['total_dps'],
                        total_workers = stats['total_workers'],
                        active_workers= stats['active_workers'],
                        dps_per_sec   = float(_live_dps_per_sec()),
                        your_share    = float(stats['your_share']),
                        your_dps      = stats['your_dps'],
                        uptime_seconds= int(time.time() - _server_start),
                    )
                    await send_frame(writer, frame)

                elif msg_type == SOLUTION:
                    try:
                        privkey_hex = parse_solution(payload)
                    except ValueError as e:
                        logger.error("Bad SOLUTION payload from %s: %s", worker_name, e)
                        continue

                    logger.critical("PUZZLE #135 KEY FOUND by %s: %s", worker_name, privkey_hex)

                    async with aiosqlite.connect(str(DB_PATH)) as sol_db:
                        await sol_db.execute(
                            "INSERT INTO solutions (private_key, found_by, work_id) VALUES (?,?,?)",
                            (privkey_hex, worker_name, session.current_work_id),
                        )
                        await sol_db.commit()

                    with open(FOUND_PATH, "a") as f:
                        f.write(f"PUZZLE #135 SOLVED!\n"
                                f"Key: {privkey_hex}\n"
                                f"By:  {worker_name}\n"
                                f"At:  {time.time()}\n\n")

                    # Broadcast solution back to confirm receipt
                    await send_frame(writer, pack_solution_notify(privkey_hex))

                else:
                    logger.debug("Unknown msg_type 0x%02x from %s", msg_type, worker_name)

    except asyncio.IncompleteReadError:
        pass
    except ConnectionResetError:
        pass
    except Exception as e:
        logger.warning("Connection error with %s (%s): %s", session.worker_name, peer_str, e)
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass
        logger.info("Disconnected: %s (%s)", session.worker_name or "(unauthenticated)", peer_str)


# ── Entry point ───────────────────────────────────────────────────────────────
async def main(host: str, port: int, tls_cert: str | None, tls_key: str | None):
    await ensure_db()

    ssl_ctx = None
    if tls_cert and tls_key:
        ssl_ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        ssl_ctx.load_cert_chain(tls_cert, tls_key)
        ssl_ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        logger.info("TLS enabled — cert=%s", tls_cert)
    else:
        logger.warning("TLS disabled — plaintext connections only. "
                       "For internet/Vast.ai use, provide --cert and --key.")

    server = await asyncio.start_server(
        handle_connection, host, port, ssl=ssl_ctx
    )
    proto = "jlps" if ssl_ctx else "jlp"
    addrs = [str(s.getsockname()) for s in server.sockets]
    logger.info("JLP Pool Server listening on %s  (%s://host:%d)", addrs, proto, port)
    logger.info("Starminer workers: --pool %s://YOUR_HOST:%d --worker YOUR_BTC_ADDR", proto, port)
    logger.info("Vast.ai workers:   --pool %s://$(curl -s ifconfig.me):%d ...", proto, port)
    logger.info("DB: %s", DB_PATH)

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="JLP Kangaroo Pool Server for Puzzle #135")
    ap.add_argument("--host",  default="0.0.0.0",  help="Bind address (default: 0.0.0.0)")
    ap.add_argument("--port",  type=int, default=17403, help="Port (default: 17403)")
    ap.add_argument("--cert",  default=None, help="TLS certificate (PEM)")
    ap.add_argument("--key",   default=None, help="TLS private key (PEM)")
    ap.add_argument("--db",    default=None, help="Override DB path")
    ap.add_argument("-v", "--verbose", action="store_true", help="Debug logging")
    args = ap.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    if args.db:
        DB_PATH = Path(args.db)

    asyncio.run(main(args.host, args.port, args.cert, args.key))
