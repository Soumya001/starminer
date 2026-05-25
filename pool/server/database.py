"""SQLite persistence for Puzzle #135 Kangaroo pool."""

import time
import aiosqlite

_DB_PATH = "puzzle135.db"
_db: aiosqlite.Connection | None = None


async def init_db(path: str = _DB_PATH):
    global _DB_PATH, _db
    _DB_PATH = path
    _db = await aiosqlite.connect(_DB_PATH)
    _db.row_factory = aiosqlite.Row
    await _db.execute("PRAGMA journal_mode=WAL")
    await _db.execute("PRAGMA synchronous=NORMAL")
    await _db.execute("PRAGMA foreign_keys=ON")
    await _db.executescript("""
        CREATE TABLE IF NOT EXISTS workers (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            name        TEXT    NOT NULL UNIQUE,
            wallet      TEXT    NOT NULL DEFAULT '',
            version     TEXT    NOT NULL DEFAULT '',
            created_at  REAL    NOT NULL,
            last_seen   REAL    NOT NULL
        );

        CREATE TABLE IF NOT EXISTS sessions (
            token       TEXT    PRIMARY KEY,
            worker_id   INTEGER NOT NULL REFERENCES workers(id),
            created_at  REAL    NOT NULL,
            last_seen   REAL    NOT NULL
        );

        CREATE TABLE IF NOT EXISTS work_assignments (
            work_id         INTEGER PRIMARY KEY AUTOINCREMENT,
            worker_id       INTEGER NOT NULL REFERENCES workers(id),
            range_start     TEXT    NOT NULL,
            range_end       TEXT    NOT NULL,
            dp_bits         INTEGER NOT NULL,
            status          TEXT    NOT NULL DEFAULT 'active',
            assigned_at     REAL    NOT NULL,
            deadline        REAL    NOT NULL,
            last_ping       REAL    NOT NULL,
            completed_at    REAL
        );

        CREATE TABLE IF NOT EXISTS distinguished_points (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            work_id     INTEGER NOT NULL REFERENCES work_assignments(work_id),
            worker_id   INTEGER NOT NULL REFERENCES workers(id),
            x           TEXT    NOT NULL,
            d           TEXT    NOT NULL,
            type        INTEGER NOT NULL,
            sequence    INTEGER NOT NULL,
            dp_bits     INTEGER NOT NULL,
            received_at REAL    NOT NULL
        );

        CREATE TABLE IF NOT EXISTS worker_stats (
            worker_id       INTEGER PRIMARY KEY REFERENCES workers(id),
            total_dps       INTEGER NOT NULL DEFAULT 0,
            total_work      INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS solution (
            id          INTEGER PRIMARY KEY,
            private_key TEXT    NOT NULL,
            worker_id   INTEGER NOT NULL REFERENCES workers(id),
            work_id     INTEGER,
            found_at    REAL    NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_dp_x ON distinguished_points(x);
        CREATE INDEX IF NOT EXISTS idx_dp_work ON distinguished_points(work_id);
        CREATE INDEX IF NOT EXISTS idx_wa_status ON work_assignments(status);
        CREATE INDEX IF NOT EXISTS idx_sessions_worker ON sessions(worker_id);
    """)
    await _db.commit()


def _get_db() -> aiosqlite.Connection:
    if _db is None:
        raise RuntimeError("Database not initialized")
    return _db


async def close_db():
    global _db
    if _db:
        await _db.close()
        _db = None


# ---------------------------------------------------------------------------
# Workers / sessions
# ---------------------------------------------------------------------------

async def upsert_worker(name: str, wallet: str, version: str) -> int:
    db = _get_db()
    now = time.time()
    await db.execute(
        """INSERT INTO workers(name, wallet, version, created_at, last_seen)
           VALUES(?, ?, ?, ?, ?)
           ON CONFLICT(name) DO UPDATE SET
               wallet   = CASE WHEN excluded.wallet != '' THEN excluded.wallet ELSE wallet END,
               version  = excluded.version,
               last_seen = excluded.last_seen""",
        (name, wallet, version, now, now),
    )
    await db.commit()
    async with db.execute("SELECT id FROM workers WHERE name = ?", (name,)) as cur:
        row = await cur.fetchone()
    return row["id"]


async def create_session(worker_id: int, token: str):
    db = _get_db()
    now = time.time()
    await db.execute(
        "INSERT OR REPLACE INTO sessions(token, worker_id, created_at, last_seen) VALUES(?, ?, ?, ?)",
        (token, worker_id, now, now),
    )
    await db.commit()


async def get_session(token: str) -> dict | None:
    db = _get_db()
    async with db.execute(
        "SELECT s.worker_id, w.name, w.wallet FROM sessions s JOIN workers w ON w.id = s.worker_id WHERE s.token = ?",
        (token,),
    ) as cur:
        row = await cur.fetchone()
    if row is None:
        return None
    return {"worker_id": row["worker_id"], "name": row["name"], "wallet": row["wallet"]}


async def touch_session(token: str):
    db = _get_db()
    await db.execute("UPDATE sessions SET last_seen = ? WHERE token = ?", (time.time(), token))
    await db.commit()


# ---------------------------------------------------------------------------
# Work assignments
# ---------------------------------------------------------------------------

async def create_work(worker_id: int, range_start: str, range_end: str,
                      dp_bits: int, deadline: float) -> int:
    db = _get_db()
    now = time.time()
    cur = await db.execute(
        """INSERT INTO work_assignments(worker_id, range_start, range_end, dp_bits,
                                        status, assigned_at, deadline, last_ping)
           VALUES(?, ?, ?, ?, 'active', ?, ?, ?)""",
        (worker_id, range_start, range_end, dp_bits, now, deadline, now),
    )
    await db.commit()
    return cur.lastrowid


async def ping_work(work_id: int, new_deadline: float):
    db = _get_db()
    now = time.time()
    await db.execute(
        "UPDATE work_assignments SET last_ping = ?, deadline = ? WHERE work_id = ? AND status = 'active'",
        (now, new_deadline, work_id),
    )
    await db.commit()


async def complete_work(work_id: int):
    db = _get_db()
    await db.execute(
        "UPDATE work_assignments SET status = 'complete', completed_at = ? WHERE work_id = ?",
        (time.time(), work_id),
    )
    await db.commit()


async def expire_stale_work(before: float) -> int:
    db = _get_db()
    cur = await db.execute(
        "UPDATE work_assignments SET status = 'expired' WHERE status = 'active' AND deadline < ?",
        (before,),
    )
    await db.commit()
    return cur.rowcount


async def get_active_work_ids() -> set[int]:
    db = _get_db()
    async with db.execute("SELECT work_id FROM work_assignments WHERE status = 'active'") as cur:
        rows = await cur.fetchall()
    return {r["work_id"] for r in rows}


# ---------------------------------------------------------------------------
# Distinguished points
# ---------------------------------------------------------------------------

async def save_dps(work_id: int, worker_id: int, dps: list[dict]) -> int:
    """Bulk-insert DPs. Returns count inserted."""
    db = _get_db()
    now = time.time()
    rows = [
        (work_id, worker_id, dp["x"], dp["d"], dp["type"],
         dp.get("sequence", 0), dp.get("dp_bits", 0), now)
        for dp in dps
    ]
    await db.executemany(
        """INSERT INTO distinguished_points
           (work_id, worker_id, x, d, type, sequence, dp_bits, received_at)
           VALUES(?, ?, ?, ?, ?, ?, ?, ?)""",
        rows,
    )
    await db.commit()
    return len(rows)


async def find_collision(x: str) -> dict | None:
    """Return the first DP with the same x but opposite type, or None."""
    db = _get_db()
    async with db.execute(
        "SELECT x, d, type, work_id, worker_id FROM distinguished_points WHERE x = ? LIMIT 2",
        (x,),
    ) as cur:
        rows = await cur.fetchall()
    if len(rows) < 2:
        return None
    if rows[0]["type"] != rows[1]["type"]:
        return {"a": dict(rows[0]), "b": dict(rows[1])}
    return None


async def total_dps() -> int:
    db = _get_db()
    async with db.execute("SELECT COUNT(*) as cnt FROM distinguished_points") as cur:
        row = await cur.fetchone()
    return row["cnt"]


# ---------------------------------------------------------------------------
# Worker stats
# ---------------------------------------------------------------------------

async def increment_worker_stats(worker_id: int, dp_count: int, work_count: int = 0):
    db = _get_db()
    await db.execute(
        """INSERT INTO worker_stats(worker_id, total_dps, total_work)
           VALUES(?, ?, ?)
           ON CONFLICT(worker_id) DO UPDATE SET
               total_dps  = total_dps  + excluded.total_dps,
               total_work = total_work + excluded.total_work""",
        (worker_id, dp_count, work_count),
    )
    await db.commit()


async def get_leaderboard(limit: int = 50) -> list[dict]:
    db = _get_db()
    async with db.execute(
        """SELECT w.name, w.wallet, w.last_seen,
                  COALESCE(ws.total_dps, 0)  AS total_dps,
                  COALESCE(ws.total_work, 0) AS total_work
           FROM workers w
           LEFT JOIN worker_stats ws ON ws.worker_id = w.id
           ORDER BY total_dps DESC
           LIMIT ?""",
        (limit,),
    ) as cur:
        rows = await cur.fetchall()
    return [dict(r) for r in rows]


async def get_pool_stats() -> dict:
    db = _get_db()
    now = time.time()
    active_cutoff = now - 300  # active = pinged within last 5 min

    async with db.execute("SELECT COUNT(*) as cnt FROM workers") as cur:
        total_workers = (await cur.fetchone())["cnt"]

    async with db.execute(
        "SELECT COUNT(DISTINCT worker_id) as cnt FROM work_assignments WHERE status = 'active'"
    ) as cur:
        active_workers = (await cur.fetchone())["cnt"]

    async with db.execute("SELECT COUNT(*) as cnt FROM work_assignments WHERE status = 'active'") as cur:
        active_assignments = (await cur.fetchone())["cnt"]

    async with db.execute("SELECT COUNT(*) as cnt FROM distinguished_points") as cur:
        dp_count = (await cur.fetchone())["cnt"]

    async with db.execute("SELECT COUNT(*) as cnt FROM work_assignments WHERE status = 'complete'") as cur:
        completed_work = (await cur.fetchone())["cnt"]

    async with db.execute("SELECT COUNT(*) as cnt FROM solution") as cur:
        solution_found = (await cur.fetchone())["cnt"] > 0

    return {
        "total_workers": total_workers,
        "active_workers": active_workers,
        "active_assignments": active_assignments,
        "total_dps": dp_count,
        "completed_work": completed_work,
        "solution_found": solution_found,
    }


# ---------------------------------------------------------------------------
# Solution
# ---------------------------------------------------------------------------

async def record_solution(private_key: str, worker_id: int, work_id: int | None):
    db = _get_db()
    await db.execute(
        "INSERT OR IGNORE INTO solution(id, private_key, worker_id, work_id, found_at) VALUES(1, ?, ?, ?, ?)",
        (private_key, worker_id, work_id, time.time()),
    )
    await db.commit()


async def get_solution() -> dict | None:
    db = _get_db()
    async with db.execute("SELECT * FROM solution WHERE id = 1") as cur:
        row = await cur.fetchone()
    return dict(row) if row else None
