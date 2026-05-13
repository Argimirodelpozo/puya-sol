"""Auto-generated tests for the freeFunctions category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_easy(harness):
    """freeFunctions/easy.sol"""
    app = harness.compile_and_deploy("freeFunctions/easy.sol")
    # f(uint256): 7 -> 9
    r = harness.call(app, "f(uint256)", 7)
    assert r.abi_return == 9

def test_free_namesake_contract_function(harness):
    """freeFunctions/free_namesake_contract_function.sol"""
    app = harness.compile_and_deploy("freeFunctions/free_namesake_contract_function.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_free_runtimecode(harness):
    """freeFunctions/free_runtimecode.sol"""
    app = harness.compile_and_deploy("freeFunctions/free_runtimecode.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_import_(harness):
    """freeFunctions/import.sol"""
    app = harness.compile_and_deploy("freeFunctions/import.sol")
    # f(uint256): 7 -> 7, 8
    r = harness.call(app, "f(uint256)", 7)
    assert tuple(r.abi_return) == (7, 8)

def test_libraries_from_free(harness):
    """freeFunctions/libraries_from_free.sol"""
    app = harness.compile_and_deploy("freeFunctions/libraries_from_free.sol")
    # f() -> 7, 8
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (7, 8)

def test_new_operator(harness):
    """freeFunctions/new_operator.sol"""
    app = harness.compile_and_deploy("freeFunctions/new_operator.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_overloads(harness):
    """freeFunctions/overloads.sol"""
    app = harness.compile_and_deploy("freeFunctions/overloads.sol")
    # g() -> 2, 3
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (2, 3)

def test_recursion(harness):
    """freeFunctions/recursion.sol"""
    app = harness.compile_and_deploy("freeFunctions/recursion.sol")
    # g(uint256,uint256): 0, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 0, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 0, 1 -> 0x00
    r = harness.call(app, "g(uint256,uint256)", 0, 1)
    assert r.abi_return == 0
    # g(uint256,uint256): 1, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 0)
    assert r.abi_return == 1
    # g(uint256,uint256): 2, 3 -> 8
    r = harness.call(app, "g(uint256,uint256)", 2, 3)
    assert r.abi_return == 8
    # g(uint256,uint256): 3, 10 -> 59049
    r = harness.call(app, "g(uint256,uint256)", 3, 10)
    assert r.abi_return == 59049
    # g(uint256,uint256): 2, 255 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "g(uint256,uint256)", 2, 255)
    assert r.abi_return == -57896044618658097711785492504343953926634992332820282019728792003956564819968

def test_storage_calldata_refs(harness):
    """freeFunctions/storage_calldata_refs.sol"""
    app = harness.compile_and_deploy("freeFunctions/storage_calldata_refs.sol")
    # f(uint256,uint256[]): 7, 0x40, 3, 8, 9, 10 -> 7, 9
    r = harness.call(app, "f(uint256,uint256[])", 7, 64, 3, 8, 9, 10)
    assert tuple(r.abi_return) == (7, 9)
