"""Tests for the saltedCreate category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_prediction_example(harness):
    """saltedCreate/contracts/prediction_example.sol"""
    app = harness.compile_and_deploy("saltedCreate/contracts/prediction_example.sol")
    # bytes32 salt = 0x000...2a, uint256 arg = 64.
    salt = b"\x00" * 31 + b"\x2a"
    r = harness.call(app, "createDSalted(bytes32,uint256)", salt, 64)
    assert not r.reverted

def test_salted_create(harness):
    """saltedCreate/contracts/salted_create.sol"""
    app = harness.compile_and_deploy("saltedCreate/contracts/salted_create.sol")
    # different_salt() -> true
    r = harness.call(app, "different_salt()")
    assert bool(as_int(r.abi_return)) is True
    # same_salt() -> true
    r = harness.call(app, "same_salt()")
    assert bool(as_int(r.abi_return)) is True

def test_salted_create_with_value(harness):
    """saltedCreate/contracts/salted_create_with_value.sol"""
    app = harness.compile_and_deploy("saltedCreate/contracts/salted_create_with_value.sol")
    # f(), 10 ether -> 3007, 3008, 3009
    r = harness.call(app, "f()", payment_wei=10000000000000000000)
    assert tuple(as_int(x) for x in r.abi_return) == (3007, 3008, 3009)
