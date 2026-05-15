"""Tests for the reverts category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_assert_require(harness):
    """reverts/contracts/assert_require.sol"""
    app = harness.compile_and_deploy("reverts/contracts/assert_require.sol")
    # f() -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g(bool): false -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "g(bool)", False, expect_revert=True)
    assert r.reverted
    # g(bool): true -> true
    r = harness.call(app, "g(bool)", True)
    assert bool(as_int(r.abi_return)) is True
    # h(bool): false -> FAILURE
    r = harness.call(app, "h(bool)", False, expect_revert=True)
    assert r.reverted
    # h(bool): true -> true
    r = harness.call(app, "h(bool)", True)
    assert bool(as_int(r.abi_return)) is True

def test_error_struct(harness):
    """reverts/contracts/error_struct.sol"""
    app = harness.compile_and_deploy("reverts/contracts/error_struct.sol")
    # f() -> FAILURE, hex"f8a8fd6d"
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g(uint256): 7 -> 7
    r = harness.call(app, "g(uint256)", 7)
    assert as_int(r.abi_return) == 7

def test_invalid_enum_as_external_arg(harness):
    """reverts/contracts/invalid_enum_as_external_arg.sol"""
    app = harness.compile_and_deploy("reverts/contracts/invalid_enum_as_external_arg.sol")
    # test() -> FAILURE, hex"4e487b71", 0x21 # should throw #
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_invalid_enum_as_external_ret(harness):
    """reverts/contracts/invalid_enum_as_external_ret.sol"""
    app = harness.compile_and_deploy("reverts/contracts/invalid_enum_as_external_ret.sol")
    # test_return() -> FAILURE, hex"4e487b71", 33 # both should throw #
    r = harness.call(app, "test_return()", expect_revert=True)
    assert r.reverted
    # test_inline_assignment() -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "test_inline_assignment()", expect_revert=True)
    assert r.reverted
    # test_assignment() -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "test_assignment()", expect_revert=True)
    assert r.reverted

def test_invalid_enum_compared(harness):
    """reverts/contracts/invalid_enum_compared.sol"""
    app = harness.compile_and_deploy("reverts/contracts/invalid_enum_compared.sol")
    # test_eq_ok() -> 1
    r = harness.call(app, "test_eq_ok()")
    assert as_int(r.abi_return) == 1
    # test_eq() -> FAILURE, hex"4e487b71", 33 # both should throw #
    r = harness.call(app, "test_eq()", expect_revert=True)
    assert r.reverted
    # test_neq() -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "test_neq()", expect_revert=True)
    assert r.reverted

def test_invalid_enum_stored(harness):
    """reverts/contracts/invalid_enum_stored.sol"""
    app = harness.compile_and_deploy("reverts/contracts/invalid_enum_stored.sol")
    # test_store_ok() -> 1
    r = harness.call(app, "test_store_ok()")
    assert as_int(r.abi_return) == 1
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # test_store() -> FAILURE, hex"4e487b71", 33 # should throw #
    r = harness.call(app, "test_store()", expect_revert=True)
    assert r.reverted

def test_invalid_instruction(harness):
    """reverts/contracts/invalid_instruction.sol"""
    app = harness.compile_and_deploy("reverts/contracts/invalid_instruction.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_revert(harness):
    """reverts/contracts/revert.sol"""
    app = harness.compile_and_deploy("reverts/contracts/revert.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # a() -> 42
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 42
    # g() -> FAILURE
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted
    # a() -> 42
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 42

def test_revert_return_area(harness):
    """reverts/contracts/revert_return_area.sol"""
    app = harness.compile_and_deploy('reverts/contracts/revert_return_area.sol')
    r = harness.call(app, 'f()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x00, 0x08c379a000000000000000000000000000000000000000000000000000000000,)

def test_simple_throw(harness):
    """reverts/contracts/simple_throw.sol"""
    app = harness.compile_and_deploy("reverts/contracts/simple_throw.sol")
    # f(uint256): 11 -> 21
    r = harness.call(app, "f(uint256)", 11)
    assert as_int(r.abi_return) == 21
    # f(uint256): 1 -> FAILURE
    r = harness.call(app, "f(uint256)", 1, expect_revert=True)
    assert r.reverted
