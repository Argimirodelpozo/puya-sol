"""Tests for the uninitializedFunctionPointer category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_invalidInConstructor(harness):
    """uninitializedFunctionPointer/contracts/invalidInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/contracts/invalidInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_invalidStoredInConstructor(harness):
    """uninitializedFunctionPointer/contracts/invalidStoredInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/contracts/invalidStoredInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_store2(harness):
    """uninitializedFunctionPointer/contracts/store2.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/contracts/store2.sol")
    # run() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "run()", expect_revert=True)
    assert r.reverted

def test_storeInConstructor(harness):
    """uninitializedFunctionPointer/contracts/storeInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/contracts/storeInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_uninitialized_internal_storage_function_legacy(harness):
    """uninitializedFunctionPointer/contracts/uninitialized_internal_storage_function_legacy.sol"""
    pytest.fail("EVM legacy codegen: uninitialized internal fn pointer hits panic tag. AVM has no equivalent panic-tag dispatch.")

def test_uninitialized_internal_storage_function_via_yul(harness):
    """uninitializedFunctionPointer/contracts/uninitialized_internal_storage_function_via_yul.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/contracts/uninitialized_internal_storage_function_via_yul.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
