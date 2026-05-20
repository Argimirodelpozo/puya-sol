"""Value helpers for Solidity isoltest semantics.

Solidity's semantic test format expresses padded values explicitly:
    left(0x4200ef)   → 0x4200ef00...00  (right-padded to 32 bytes)
    right(0x4200ef)  → 0x00...004200ef  (left-padded to 32 bytes)

In test functions written against the new framework, the assertion side
holds typed Python values. These helpers exist for the rare case where
a test really does want a specific 32-byte layout (typically `bytes32`
state vars whose raw bytes layout matters).
"""
from __future__ import annotations


def lpad(value: bytes | int, width: int = 32) -> bytes:
    """Left-pad value to `width` bytes (typical Solidity uint encoding)."""
    if isinstance(value, int):
        return value.to_bytes(width, "big", signed=False)
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"lpad expects bytes or int, got {type(value).__name__}")
    if len(value) > width:
        raise ValueError(f"value too long: {len(value)} > {width}")
    return b"\x00" * (width - len(value)) + bytes(value)


def rpad(value: bytes | str, width: int = 32) -> bytes:
    """Right-pad value to `width` bytes (typical Solidity bytesN/string encoding)."""
    if isinstance(value, str):
        value = value.encode()
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"rpad expects bytes or str, got {type(value).__name__}")
    if len(value) > width:
        raise ValueError(f"value too long: {len(value)} > {width}")
    return bytes(value) + b"\x00" * (width - len(value))


def u256(value: int) -> int:
    """Wrap value to unsigned 256-bit (mod 2**256)."""
    return value & ((1 << 256) - 1)


def i256(value: int) -> int:
    """Encode signed Python int as two's-complement uint256."""
    if value >= 0:
        return value & ((1 << 256) - 1)
    return ((1 << 256) + value) & ((1 << 256) - 1)


def hex_bytes(s: str) -> bytes:
    """`hex_bytes("0x4200ef")` or `hex_bytes("4200ef")` → bytes."""
    if s.startswith("0x"):
        s = s[2:]
    if len(s) % 2 == 1:
        s += "0"  # right-pad odd-length hex like Solidity does
    return bytes.fromhex(s)


def str_bytes(s: str) -> bytes:
    """ASCII encode a Python string."""
    return s.encode("ascii")


def raw(value: bytes) -> bytes:
    """Identity marker. Used in test code to signal 'this is the raw byte sequence
    the contract returns; do not interpret it as ABI-decoded'."""
    return value


def as_signed_int(value, bits: int = 256) -> int:
    """Coerce algosdk's typed return into a signed Python int.

    Use when a Solidity function declared `returns (int<N>)` (or any
    signed integer type) but our ABI surfacing emits the value as a
    32-byte uint256 (two's-complement payload). algosdk decodes that
    as a positive biguint; this helper reinterprets the top `bits`
    bits as signed two's complement.

    Examples:
        as_signed_int(2**256 - 5)        → -5     (default 256 bits)
        as_signed_int(0xfb, bits=8)      → -5
        as_signed_int(42)                →  42

    When the value is already a negative Python int (e.g. algokit's
    patched SignedIntType returned `int8` natively), it is passed
    through unchanged.
    """
    if isinstance(value, int) and not isinstance(value, bool):
        if value < 0:
            return value
        n = value
    else:
        n = as_int(value)
    sign_bit = 1 << (bits - 1)
    if n & sign_bit:
        n -= 1 << bits
    return n


def as_int(value) -> int:
    """Coerce algosdk's typed return into the equivalent uint256 int.

    Use when a Solidity function declared `returns (bytes32)` (or any
    byte[N]) but the test expectation is written as an int. algosdk
    delivers byte[N] as `list[int]`, so a strict `== 0x...` check fails
    against `[0,0,...]`.

    Conversions:
      int          → unchanged
      bool         → 0 / 1
      bytes        → big-endian int.from_bytes
      list/tuple   → bytes(...) then int.from_bytes
      str (algo)   → decode_address → 32-byte int (last 20 bytes match an EVM address)
    """
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, (bytes, bytearray)):
        return int.from_bytes(value, "big")
    if isinstance(value, (list, tuple)):
        # Filter out non-int items (defensive) — algosdk delivers byte[N] as
        # a list[int] in [0, 255].
        if all(isinstance(x, int) for x in value):
            return int.from_bytes(bytes(value), "big")
    if isinstance(value, str):
        # Algorand address (58-char base32). decode_address yields 32 bytes.
        from algosdk import encoding as _enc
        try:
            return int.from_bytes(_enc.decode_address(value), "big")
        except Exception:
            pass
    raise TypeError(f"can't coerce {type(value).__name__} to int: {value!r}")


def as_bytes(value) -> bytes:
    """Coerce algosdk's typed return into a bytes object.

    int → 32-byte big-endian. bytes → unchanged. list[int]/tuple[int] →
    bytes(...). str → utf-8 encode (for `string` returns).
    """
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, (list, tuple)):
        return bytes(value)
    if isinstance(value, int):
        return value.to_bytes(32, "big")
    if isinstance(value, str):
        return value.encode("utf-8")
    raise TypeError(f"can't coerce {type(value).__name__} to bytes: {value!r}")
