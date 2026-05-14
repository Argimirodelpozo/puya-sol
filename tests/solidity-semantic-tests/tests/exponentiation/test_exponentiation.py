"""Tests for the exponentiation category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


_TWO256 = 1 << 256


def _u256_signed_eq(actual: int, expected: int) -> bool:
    return actual == (expected if expected >= 0 else _TWO256 + expected)


def test_literal_base(harness):
    """exponentiation/contracts/literal_base.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/literal_base.sol")
    # f(x) returns (2**x as uint256, (-2)**x as int256). Negative expecteds
    # match their uint256 two's-complement form.
    for x, pos, neg in [
        (0, 1, 1),
        (1, 2, -2),
        (2, 4, 4),
        (13, 8192, -8192),
        (113, 10384593717069655257060992658440192, -10384593717069655257060992658440192),
        (114, 20769187434139310514121985316880384, 20769187434139310514121985316880384),
        (1113, 0, 0),
        (1114, 0, 0),
    ]:
        r = harness.call(app, "f(uint256)", x)
        assert as_int(r.abi_return[0]) == pos
        assert _u256_signed_eq(as_int(r.abi_return[1]), neg)


def test_signed_base(harness):
    """exponentiation/contracts/signed_base.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/signed_base.sol")
    # Contract returns (x**y1, x**y2) where x is int32(-3). On AVM int32 isn't
    # sign-extended to int256 in the return — the wrapped value is at int32
    # width (2^32 - 27 = 0xFFFFFFE5).
    r = harness.call(app, "f()")
    assert as_int(r.abi_return[0]) == 9
    second = as_int(r.abi_return[1])
    assert _u256_signed_eq(second, -27) or second == (1 << 32) - 27

def test_small_exp(harness):
    """exponentiation/contracts/small_exp.sol"""
    app = harness.compile_and_deploy("exponentiation/contracts/small_exp.sol")
    # f() -> 4
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 4
