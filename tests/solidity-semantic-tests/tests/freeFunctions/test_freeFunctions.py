"""Tests for the freeFunctions category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_easy(harness):
    """freeFunctions/contracts/easy.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/easy.sol")
    # f(uint256): 7 -> 9
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 9

def test_free_namesake_contract_function(harness):
    """freeFunctions/contracts/free_namesake_contract_function.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/free_namesake_contract_function.sol")
    # f() -> FAILURE
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted

def test_free_runtimecode(harness):
    """freeFunctions/contracts/free_runtimecode.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/free_runtimecode.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_import_(harness):
    """freeFunctions/contracts/import.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/import.sol")
    # f(uint256): 7 -> 7, 8
    r = harness.call(app, "f(uint256)", 7)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)

def test_libraries_from_free(harness):
    """freeFunctions/contracts/libraries_from_free.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/libraries_from_free.sol")
    # f() -> 7, 8
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)

def test_new_operator(harness):
    """freeFunctions/contracts/new_operator.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/new_operator.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_overloads(harness):
    """freeFunctions/contracts/overloads.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/overloads.sol")
    # g() -> 2, 3
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 3)

def test_recursion(harness):
    """freeFunctions/contracts/recursion.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/recursion.sol")
    # g(uint256,uint256): 0, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 0, 0)
    assert as_int(r.abi_return) == 1
    # g(uint256,uint256): 0, 1 -> 0x00
    r = harness.call(app, "g(uint256,uint256)", 0, 1)
    assert as_int(r.abi_return) == 0
    # g(uint256,uint256): 1, 0 -> 1
    r = harness.call(app, "g(uint256,uint256)", 1, 0)
    assert as_int(r.abi_return) == 1
    # g(uint256,uint256): 2, 3 -> 8
    r = harness.call(app, "g(uint256,uint256)", 2, 3)
    assert as_int(r.abi_return) == 8
    # g(uint256,uint256): 3, 10 -> 59049
    r = harness.call(app, "g(uint256,uint256)", 3, 10)
    assert as_int(r.abi_return) == 59049
    # g(uint256,uint256): 2, 255 -> -57896044618658097711785492504343953926634992332820282019728792003956564819968
    r = harness.call(app, "g(uint256,uint256)", 2, 255)
    assert as_int(r.abi_return) in (-57896044618658097711785492504343953926634992332820282019728792003956564819968, 57896044618658097711785492504343953926634992332820282019728792003956564819968)

def test_storage_calldata_refs(harness):
    """freeFunctions/contracts/storage_calldata_refs.sol"""
    app = harness.compile_and_deploy("freeFunctions/contracts/storage_calldata_refs.sol")
    r = harness.call(app, "f(uint256,uint256[])", 7, [8, 9, 10])
    assert tuple(as_int(x) for x in r.abi_return) == (7, 9)
