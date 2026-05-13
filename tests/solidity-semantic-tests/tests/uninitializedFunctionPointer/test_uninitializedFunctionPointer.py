"""Auto-generated tests for the uninitializedFunctionPointer category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_invalidInConstructor(harness):
    """uninitializedFunctionPointer/invalidInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/invalidInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_invalidStoredInConstructor(harness):
    """uninitializedFunctionPointer/invalidStoredInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/invalidStoredInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_store2(harness):
    """uninitializedFunctionPointer/store2.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/store2.sol")
    # run() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "run()", expect_revert=True)
    assert r.reverted

def test_storeInConstructor(harness):
    """uninitializedFunctionPointer/storeInConstructor.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/storeInConstructor.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_uninitialized_internal_storage_function_legacy(harness):
    """uninitializedFunctionPointer/uninitialized_internal_storage_function_legacy.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/uninitialized_internal_storage_function_legacy.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_uninitialized_internal_storage_function_via_yul(harness):
    """uninitializedFunctionPointer/uninitialized_internal_storage_function_via_yul.sol"""
    app = harness.compile_and_deploy("uninitializedFunctionPointer/uninitialized_internal_storage_function_via_yul.sol", via_yul_behavior=True)
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True
