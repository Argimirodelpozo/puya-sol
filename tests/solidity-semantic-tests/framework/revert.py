"""Revert reason classifiers used in test expectations.

Solidity reverts encode the reason as ABI-encoded calldata:
    keccak("Error(string)")[:4]  ++ abi.encode(string)   → ErrorString
    keccak("Panic(uint256)")[:4] ++ abi.encode(uint256)  → Panic
    (anything else, including empty)                     → RawRevert

`Reverted` is a catch-all for tests that only care that the call
reverted, regardless of reason.
"""
from __future__ import annotations

from dataclasses import dataclass


ERROR_SELECTOR = bytes.fromhex("08c379a0")  # keccak256("Error(string)")[:4]
PANIC_SELECTOR = bytes.fromhex("4e487b71")  # keccak256("Panic(uint256)")[:4]


@dataclass(frozen=True)
class Reverted:
    """Marker: call must revert, reason doesn't matter."""
    def matches(self, _data: bytes) -> bool:
        return True


@dataclass(frozen=True)
class ErrorString:
    """Marker: call must revert with Error(string) carrying `message`."""
    message: str

    def matches(self, data: bytes) -> bool:
        return _decode_error_string(data) == self.message


@dataclass(frozen=True)
class Panic:
    """Marker: call must revert with Panic(uint256) carrying `code`.

    Standard codes:
        0x01 = assert(false)
        0x11 = arithmetic overflow/underflow
        0x12 = divide-by-zero
        0x21 = invalid enum conversion
        0x22 = invalid storage byte array access
        0x31 = pop on empty array
        0x32 = array index out of bounds
        0x41 = out of memory / too much memory allocated
        0x51 = call to zero-initialized fn pointer
    """
    code: int

    def matches(self, data: bytes) -> bool:
        return _decode_panic_code(data) == self.code


@dataclass(frozen=True)
class RawRevert:
    """Marker: call must revert with raw bytes equal to `data`."""
    data: bytes

    def matches(self, data: bytes) -> bool:
        return data == self.data


def _decode_error_string(data: bytes) -> str | None:
    if len(data) < 4 + 64 or data[:4] != ERROR_SELECTOR:
        return None
    # offset (32) + length (32) + chunks
    length = int.from_bytes(data[4 + 32 : 4 + 64], "big")
    raw = data[4 + 64 : 4 + 64 + length]
    try:
        return raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None


def _decode_panic_code(data: bytes) -> int | None:
    if len(data) < 4 + 32 or data[:4] != PANIC_SELECTOR:
        return None
    return int.from_bytes(data[4 : 4 + 32], "big")


def classify_revert(data: bytes) -> Reverted | ErrorString | Panic | RawRevert:
    """Pick the most specific revert classifier for the given bytes."""
    msg = _decode_error_string(data)
    if msg is not None:
        return ErrorString(msg)
    code = _decode_panic_code(data)
    if code is not None:
        return Panic(code)
    return RawRevert(data)
