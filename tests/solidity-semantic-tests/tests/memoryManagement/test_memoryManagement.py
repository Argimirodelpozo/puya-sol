"""Auto-generated tests for the memoryManagement category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_assembly_access(harness):
    """memoryManagement/contracts/assembly_access.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/assembly_access.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_memory_types_initialisation(harness):
    """memoryManagement/contracts/memory_types_initialisation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/memory_types_initialisation.sol")
    # stat() -> 0, 0, 0, 0, 0
    r = harness.call(app, "stat()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0
    assert not r.reverted
    # dyn() -> 0x20, 0
    r = harness.call(app, "dyn()")
    assert tuple(r.abi_return) == (32, 0)
    # nested() -> 0x20, 0
    r = harness.call(app, "nested()")
    assert tuple(r.abi_return) == (32, 0)
    # nestedStat() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "nestedStat()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_return_variable(harness):
    """memoryManagement/contracts/return_variable.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/return_variable.sol")
    # f() -> 0x0500, 0x0500, 0x0a00
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1280, 1280, 2560)

def test_static_memory_array_allocation(harness):
    """memoryManagement/contracts/static_memory_array_allocation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/static_memory_array_allocation.sol")
    # withValue() -> 0x00
    r = harness.call(app, "withValue()")
    assert r.abi_return == 0
    # withoutValue() -> 0x0280
    r = harness.call(app, "withoutValue()")
    assert r.abi_return == 640

def test_struct_allocation(harness):
    """memoryManagement/contracts/struct_allocation.sol"""
    app = harness.compile_and_deploy("memoryManagement/contracts/struct_allocation.sol")
    # withValue() -> 0x00
    r = harness.call(app, "withValue()")
    assert r.abi_return == 0
    # withoutValue() -> 0x60
    r = harness.call(app, "withoutValue()")
    assert r.abi_return == 96
