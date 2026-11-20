"""Pack the core body into a file for the loader: {u32 'OMKC', u32 length, u32 sum of body words, u32 ABI} + body.

    python3 OMKit/tools/pack.py <core.body> <CORE.BIN>
"""
import struct
import sys
from pathlib import Path

MAGIC, ABI, MAX_LEN = 0x434B4D4F, 1, 0x300000


def pack(body: bytes) -> bytes:
    body += bytes(-len(body) % 4)
    if not 0 < len(body) <= MAX_LEN - 16:
        raise SystemExit(f"body size {len(body)} is out of range")
    total = sum(struct.unpack(f"<{len(body) // 4}I", body)) & 0xFFFFFFFF
    return struct.pack("<4I", MAGIC, len(body), total, ABI) + body


def main() -> int:
    out = pack(Path(sys.argv[1]).read_bytes())
    Path(sys.argv[2]).write_bytes(out)
    print(f"{sys.argv[2]}: {len(out)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
