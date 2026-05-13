"""Auto-generated tests for the deployedCodeExclusion category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_bound_function(harness):
    """deployedCodeExclusion/bound_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/bound_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_library_function(harness):
    """deployedCodeExclusion/library_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/library_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_library_function_deployed(harness):
    """deployedCodeExclusion/library_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/library_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_module_function(harness):
    """deployedCodeExclusion/module_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/module_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_module_function_deployed(harness):
    """deployedCodeExclusion/module_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/module_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_static_base_function(harness):
    """deployedCodeExclusion/static_base_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/static_base_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_static_base_function_deployed(harness):
    """deployedCodeExclusion/static_base_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/static_base_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_subassembly_deduplication(harness):
    """deployedCodeExclusion/subassembly_deduplication.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/subassembly_deduplication.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_super_function(harness):
    """deployedCodeExclusion/super_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/super_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_super_function_deployed(harness):
    """deployedCodeExclusion/super_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/super_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_virtual_function(harness):
    """deployedCodeExclusion/virtual_function.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/virtual_function.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_virtual_function_deployed(harness):
    """deployedCodeExclusion/virtual_function_deployed.sol"""
    app = harness.compile_and_deploy("deployedCodeExclusion/virtual_function_deployed.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True
