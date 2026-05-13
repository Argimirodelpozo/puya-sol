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
