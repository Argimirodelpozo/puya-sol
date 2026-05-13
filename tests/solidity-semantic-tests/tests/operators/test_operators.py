"""Tests for the operators category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_compound_assign(harness):
    """operators/contracts/compound_assign.sol"""
    app = harness.compile_and_deploy("operators/contracts/compound_assign.sol")
    # f(uint256,uint256): 0, 6 -> 7
    r = harness.call(app, "f(uint256,uint256)", 0, 6)
    assert as_int(r.abi_return) == 7
    # f(uint256,uint256): 1, 3 -> 0x23
    r = harness.call(app, "f(uint256,uint256)", 1, 3)
    assert as_int(r.abi_return) == 35
    # f(uint256,uint256): 2, 25 -> 0x0746
    r = harness.call(app, "f(uint256,uint256)", 2, 25)
    assert as_int(r.abi_return) == 1862
    # f(uint256,uint256): 3, 69 -> 396613
    r = harness.call(app, "f(uint256,uint256)", 3, 69)
    assert as_int(r.abi_return) == 396613
    # f(uint256,uint256): 4, 84 -> 137228105
    r = harness.call(app, "f(uint256,uint256)", 4, 84)
    assert as_int(r.abi_return) == 137228105
    # f(uint256,uint256): 5, 2 -> 0xcc7c5e28
    r = harness.call(app, "f(uint256,uint256)", 5, 2)
    assert as_int(r.abi_return) == 3430702632
    # f(uint256,uint256): 6, 51 -> 1121839760671
    r = harness.call(app, "f(uint256,uint256)", 6, 51)
    assert as_int(r.abi_return) == 1121839760671
    # f(uint256,uint256): 7, 48 -> 408349672884251
    r = harness.call(app, "f(uint256,uint256)", 7, 48)
    assert as_int(r.abi_return) == 408349672884251

def test_compound_assign_transient_storage(harness):
    """operators/contracts/compound_assign_transient_storage.sol"""
    app = harness.compile_and_deploy("operators/contracts/compound_assign_transient_storage.sol")
    # f(uint256,uint256): 0, 6 -> 7
    r = harness.call(app, "f(uint256,uint256)", 0, 6)
    assert as_int(r.abi_return) == 7
    # f(uint256,uint256): 1, 3 -> 11
    r = harness.call(app, "f(uint256,uint256)", 1, 3)
    assert as_int(r.abi_return) == 11
    # f(uint256,uint256): 2, 25 -> 0x3c
    r = harness.call(app, "f(uint256,uint256)", 2, 25)
    assert as_int(r.abi_return) == 60
    # f(uint256,uint256): 3, 69 -> 0xdc
    r = harness.call(app, "f(uint256,uint256)", 3, 69)
    assert as_int(r.abi_return) == 220
    # f(uint256,uint256): 4, 84 -> 353
    r = harness.call(app, "f(uint256,uint256)", 4, 84)
    assert as_int(r.abi_return) == 353
    # f(uint256,uint256): 5, 2 -> 0x20
    r = harness.call(app, "f(uint256,uint256)", 5, 2)
    assert as_int(r.abi_return) == 32
    # f(uint256,uint256): 6, 51 -> 334
    r = harness.call(app, "f(uint256,uint256)", 6, 51)
    assert as_int(r.abi_return) == 334
    # f(uint256,uint256): 7, 48 -> 371
    r = harness.call(app, "f(uint256,uint256)", 7, 48)
    assert as_int(r.abi_return) == 371

def test_transient_storage_variable_increment_decrement(harness):
    """operators/contracts/transient_storage_variable_increment_decrement.sol"""
    app = harness.compile_and_deploy("operators/contracts/transient_storage_variable_increment_decrement.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
