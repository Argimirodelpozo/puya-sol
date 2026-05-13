"""Tests for the deployedCodeExclusion category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_bound_function(harness):
    """deployedCodeExclusion/contracts/bound_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/bound_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_library_function(harness):
    """deployedCodeExclusion/contracts/library_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/library_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_library_function_deployed(harness):
    """deployedCodeExclusion/contracts/library_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/library_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_module_function(harness):
    """deployedCodeExclusion/contracts/module_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/module_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_module_function_deployed(harness):
    """deployedCodeExclusion/contracts/module_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/module_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_static_base_function(harness):
    """deployedCodeExclusion/contracts/static_base_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/static_base_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_static_base_function_deployed(harness):
    """deployedCodeExclusion/contracts/static_base_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/static_base_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_subassembly_deduplication(harness):
    """deployedCodeExclusion/contracts/subassembly_deduplication.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/subassembly_deduplication.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_super_function(harness):
    """deployedCodeExclusion/contracts/super_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/super_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_super_function_deployed(harness):
    """deployedCodeExclusion/contracts/super_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/super_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_virtual_function(harness):
    """deployedCodeExclusion/contracts/virtual_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/virtual_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_virtual_function_deployed(harness):
    """deployedCodeExclusion/contracts/virtual_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/contracts/virtual_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True
