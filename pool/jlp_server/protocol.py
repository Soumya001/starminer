"""JLP (JeanLucPons Kangaroo Protocol) wire format — pack/unpack helpers.

Wire spec from src/pool/jlp_wire_generated.hpp (IDL hash 358fc02dc757b24f).
All integers are little-endian unless marked BE (big-endian).

Header (8 bytes):  MAGIC[4] | type[1] | flags[1] | payload_size[2 LE]
Payload size is uint16_t → max 65535 bytes per message.
"""

import struct

# ── Message types ─────────────────────────────────────────────────────────────
AUTH         = 0x01
AUTH_OK      = 0x02
AUTH_FAIL    = 0x03
WORK_REQ     = 0x10
WORK_ASN     = 0x11
DP_SUBMIT    = 0x20
DP_ACK       = 0x21
DP_BATCH     = 0x22
DP_SUBMIT_V2 = 0x23
DP_BATCH_V2  = 0x24
STATS_REQ    = 0x30
STATS_RSP    = 0x31
SOLUTION     = 0x40
PING         = 0x50
PONG         = 0x51
MSG_ERROR    = 0xff

MAGIC        = b'KANG'
HEADER_SIZE  = 8
MAX_PAYLOAD  = 65000  # client cap (uint16_t header field)
MAX_BATCH    = 10000  # server-side sanity cap

_HDR = struct.Struct('<4sBBH')   # KANG, type, flags, payload_size

# Wire sizes
AUTH_PAYLOAD_SIZE     = 96   # worker_name[64] + password[32]
WORK_ASN_SIZE         = 109  # pubkey[33] + range_start[32] + range_end[32] + dp_bits[4 LE] + work_id[8 LE]
DP_V1_SIZE            = 66   # x[32] + d[32] + type[1] + dp_bits[1]
DP_V2_SIZE            = 78   # work_id[8 LE] + sequence[4 LE] + x[32] + d[32] + type[1] + dp_bits[1]
STATS_RSP_SIZE        = 36   # QIIffQI (all LE)
SOLUTION_SIZE         = 32   # raw 32-byte private key

_DP_V2     = struct.Struct('<QI32s32sBB')   # work_id LE, seq LE, x BE, d BE, type, dp_bits
_WORK_ASN  = struct.Struct('<33s32s32sIQ')  # pubkey, range_start BE, range_end BE, dp_bits LE, work_id LE
_STATS_RSP = struct.Struct('<QIIffQI')      # total_dps, total_workers, active_workers, dps/s, your_share, your_dps, uptime


def pack_header(msg_type: int, payload_len: int) -> bytes:
    return _HDR.pack(MAGIC, msg_type, 0, payload_len)


def parse_header(data: bytes) -> tuple[int, int, int]:
    """Returns (msg_type, flags, payload_size). Raises ValueError on bad magic."""
    if len(data) < HEADER_SIZE:
        raise ValueError("Header too short")
    magic, msg_type, flags, payload_size = _HDR.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: {magic!r}")
    return msg_type, flags, payload_size


def pack_auth_ok() -> bytes:
    return pack_header(AUTH_OK, 0)


def pack_auth_fail(reason: str = "") -> bytes:
    payload = reason.encode()[:255]
    return pack_header(AUTH_FAIL, len(payload)) + payload


def pack_pong() -> bytes:
    return pack_header(PONG, 0)


def pack_dp_ack() -> bytes:
    return pack_header(DP_ACK, 0)


def pack_work_assignment(pubkey_hex: str, range_start: int, range_end: int,
                          dp_bits: int, work_id: int) -> bytes:
    pubkey_bytes = bytes.fromhex(pubkey_hex)   # 33 bytes compressed
    rs_bytes = range_start.to_bytes(32, 'big')
    re_bytes = range_end.to_bytes(32, 'big')
    payload = _WORK_ASN.pack(pubkey_bytes, rs_bytes, re_bytes, dp_bits, work_id)
    return pack_header(WORK_ASN, len(payload)) + payload


def pack_stats_rsp(total_dps: int, total_workers: int, active_workers: int,
                   dps_per_sec: float, your_share: float,
                   your_dps: int, uptime_seconds: int) -> bytes:
    payload = _STATS_RSP.pack(
        total_dps, total_workers, active_workers,
        dps_per_sec, your_share, your_dps, uptime_seconds,
    )
    return pack_header(STATS_RSP, len(payload)) + payload


def pack_solution_notify(privkey_hex: str) -> bytes:
    """Server → client: SOLUTION message (32-byte raw key)."""
    payload = bytes.fromhex(privkey_hex)[:32]
    return pack_header(SOLUTION, len(payload)) + payload


def parse_auth_payload(data: bytes) -> tuple[str, str]:
    """Returns (worker_name, password) decoded from the 96-byte AUTH payload."""
    if len(data) < AUTH_PAYLOAD_SIZE:
        raise ValueError("AUTH payload too short")
    worker_name = data[0:64].rstrip(b'\x00').decode('utf-8', errors='replace')
    password    = data[64:96].rstrip(b'\x00').decode('utf-8', errors='replace')
    return worker_name, password


def parse_dp_batch_v2(data: bytes) -> list[dict]:
    """Parse a DP_BATCH_V2 payload: [count:u32 LE][dp1:78]..."""
    if len(data) < 4:
        raise ValueError("DP_BATCH_V2 payload too short")
    count = struct.unpack_from('<I', data, 0)[0]
    if count > MAX_BATCH:
        raise ValueError(f"Batch count {count} exceeds max {MAX_BATCH}")
    expected = 4 + count * DP_V2_SIZE
    if len(data) < expected:
        raise ValueError(f"DP_BATCH_V2 truncated: need {expected}, got {len(data)}")

    dps = []
    offset = 4
    for _ in range(count):
        work_id, sequence, x_bytes, d_bytes, dp_type, dp_bits = _DP_V2.unpack_from(data, offset)
        dps.append({
            'work_id':  work_id,
            'sequence': sequence,
            'x':        x_bytes.hex(),
            'd':        d_bytes.hex(),
            'type':     dp_type,
            'dp_bits':  dp_bits,
        })
        offset += DP_V2_SIZE
    return dps


def parse_dp_v1(data: bytes) -> dict:
    """Parse a single DP_SUBMIT (v1) payload: x[32] + d[32] + type[1] + dp_bits[1]."""
    if len(data) < DP_V1_SIZE:
        raise ValueError("DP_SUBMIT payload too short")
    x_bytes = data[0:32]
    d_bytes = data[32:64]
    dp_type = data[64]
    dp_bits = data[65]
    return {'x': x_bytes.hex(), 'd': d_bytes.hex(), 'type': dp_type, 'dp_bits': dp_bits}


def parse_solution(data: bytes) -> str:
    """Parse SOLUTION payload: raw 32-byte private key → hex string."""
    if len(data) < SOLUTION_SIZE:
        raise ValueError("SOLUTION payload too short")
    return data[:32].hex()
