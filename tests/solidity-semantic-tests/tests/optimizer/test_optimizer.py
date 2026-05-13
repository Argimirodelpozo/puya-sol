"""Tests for the optimizer category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_shift_bytes(harness):
    """optimizer/contracts/shift_bytes.sol"""
    app = harness.compile_and_deploy("optimizer/contracts/shift_bytes.sol")
    # f(uint256): 0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f -> 0x1f, 0x1f, 3
    r = harness.call(app, "f(uint256)", 0x102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f)
    assert tuple(as_int(x) for x in r.abi_return) == (31, 31, 3)
    # g(uint256): 0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f -> 1, 3, 5
    r = harness.call(app, "g(uint256)", 0x102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3, 5)

def test_unused_store_storage_removal_bug(harness):
    """optimizer/contracts/unused_store_storage_removal_bug.sol"""
    app = harness.compile_and_deploy("optimizer/contracts/unused_store_storage_removal_bug.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
