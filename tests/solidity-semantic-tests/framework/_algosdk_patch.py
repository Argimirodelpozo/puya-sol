"""Monkeypatch algosdk to handle signed `int<N>` ABI types.

ARC4 / Solidity allows `int<N>` (signed) alongside `uint<N>`, but
algosdk's ABIType.from_string only handles `uint<N>` and its UintType
encoder rejects negative values.

This module:
  1. Adds a SignedIntType subclass that round-trips as `intN` and
     encodes negative Python ints as two's complement (UintType's
     decode/encode produce N raw bytes already).
  2. Patches `ABIType.from_string` to recognise `int<N>` and return
     a SignedIntType.
  3. Wraps `UintType.encode` to convert negative ints to two's
     complement (so plain `uint256` accepts -2 and encodes 2**256 - 2).
"""
from __future__ import annotations

from algosdk.abi import base_type as _base
from algosdk.abi.uint_type import UintType as _UintType


class SignedIntType(_UintType):
    """Two's complement signed integer ABI type."""

    def __str__(self) -> str:
        return f"int{self.bit_size}"

    def encode(self, value: int) -> bytes:
        if isinstance(value, bool):
            value = int(value)
        if not isinstance(value, int):
            raise TypeError(f"intN expects int, got {type(value).__name__}")
        mask = (1 << self.bit_size) - 1
        if value < 0:
            value = (value & mask)
        if value > mask:
            raise OverflowError(f"value {value} doesn't fit in int{self.bit_size}")
        return value.to_bytes(self.bit_size // 8, "big")

    def decode(self, bytestring: bytes) -> int:
        n = int.from_bytes(bytestring, "big")
        if n >> (self.bit_size - 1):
            n -= 1 << self.bit_size
        return n


_original_from_string = _base.ABIType.from_string


@staticmethod
def _patched_from_string(s: str):
    if isinstance(s, str) and s.startswith("int") and len(s) > 3 and s[3:].isdecimal():
        return SignedIntType(int(s[3:]))
    return _original_from_string(s)


_base.ABIType.from_string = _patched_from_string  # type: ignore[method-assign]


# Wrap UintType.encode to accept negative ints as two's complement (useful
# for tests that pass -1, -2, etc. as uint256 args mirroring Solidity's
# `unchecked { ... }` semantics).
_original_uint_encode = _UintType.encode


def _patched_uint_encode(self, value):
    if isinstance(value, int) and value < 0:
        value = value & ((1 << self.bit_size) - 1)
    return _original_uint_encode(self, value)


_UintType.encode = _patched_uint_encode  # type: ignore[method-assign]
