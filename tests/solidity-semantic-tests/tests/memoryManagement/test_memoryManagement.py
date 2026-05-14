"""Tests for the memoryManagement category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_assembly_access(harness):
    """memoryManagement/contracts/assembly_access.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/assembly_access.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_memory_types_initialisation(harness):
    """memoryManagement/contracts/memory_types_initialisation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/memory_types_initialisation.sol")
    # stat() returns a uint[5] memory — algosdk decodes as a 5-list of zeros.
    assert tuple(as_int(x) for x in harness.call(app, "stat()").abi_return) == (0, 0, 0, 0, 0)
    # dyn() returns an empty uint[] — algosdk decodes as empty list.
    assert list(harness.call(app, "dyn()").abi_return) == []
    # nested() returns an empty uint[3][] — empty list.
    assert list(harness.call(app, "nested()").abi_return) == []
    # nestedStat() returns a uint[3][7] memory — 21 zeros.
    r = harness.call(app, "nestedStat()")
    assert sum(as_int(y) for x in r.abi_return for y in x) == 0

def test_return_variable(harness):
    """memoryManagement/contracts/return_variable.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/return_variable.sol")
    # f() -> 0x0500, 0x0500, 0x0a00
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1280, 1280, 2560)

def test_static_memory_array_allocation(harness):
    """memoryManagement/contracts/static_memory_array_allocation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/static_memory_array_allocation.sol")
    # withValue() -> 0x00
    r = harness.call(app, "withValue()")
    assert as_int(r.abi_return) == 0
    # withoutValue() -> 0x0280
    r = harness.call(app, "withoutValue()")
    assert as_int(r.abi_return) == 640

def test_struct_allocation(harness):
    """memoryManagement/contracts/struct_allocation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/struct_allocation.sol")
    # withValue() -> 0x00
    r = harness.call(app, "withValue()")
    assert as_int(r.abi_return) == 0
    # withoutValue() -> 0x60
    r = harness.call(app, "withoutValue()")
    assert as_int(r.abi_return) == 96
