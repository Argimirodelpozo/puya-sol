"""Auto-generated tests for the multiSource category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_circular_import(harness):
    """multiSource/circular_import.sol"""
    app = harness.compile_and_deploy("multiSource/circular_import.sol")
    # foo() -> 1
    r = harness.call(app, "foo()")
    assert r.abi_return == 1

def test_circular_import_2(harness):
    """multiSource/circular_import_2.sol"""
    app = harness.compile_and_deploy("multiSource/circular_import_2.sol")
    # foo() -> 992
    r = harness.call(app, "foo()")
    assert r.abi_return == 992

def test_circular_reimport(harness):
    """multiSource/circular_reimport.sol"""
    app = harness.compile_and_deploy("multiSource/circular_reimport.sol")
    # foo() -> 0x60
    r = harness.call(app, "foo()")
    assert r.abi_return == 96

def test_circular_reimport_2(harness):
    """multiSource/circular_reimport_2.sol"""
    app = harness.compile_and_deploy("multiSource/circular_reimport_2.sol")
    # foo() -> 0x2324
    r = harness.call(app, "foo()")
    assert r.abi_return == 8996

def test_free_different_interger_types(harness):
    """multiSource/free_different_interger_types.sol"""
    app = harness.compile_and_deploy("multiSource/free_different_interger_types.sol")
    # foo() -> 24, true
    r = harness.call(app, "foo()")
    # TODO: verify expected: 24 | true
    assert not r.reverted

def test_free_function_resolution_base_contract(harness):
    """multiSource/free_function_resolution_base_contract.sol"""
    app = harness.compile_and_deploy("multiSource/free_function_resolution_base_contract.sol")
    # h() -> 1337
    r = harness.call(app, "h()")
    assert r.abi_return == 1337

def test_free_function_resolution_override_virtual(harness):
    """multiSource/free_function_resolution_override_virtual.sol"""
    app = harness.compile_and_deploy("multiSource/free_function_resolution_override_virtual.sol")
    # g() -> 1337
    r = harness.call(app, "g()")
    assert r.abi_return == 1337

def test_free_function_resolution_override_virtual_super(harness):
    """multiSource/free_function_resolution_override_virtual_super.sol"""
    app = harness.compile_and_deploy("multiSource/free_function_resolution_override_virtual_super.sol")
    # g() -> 1337
    r = harness.call(app, "g()")
    assert r.abi_return == 1337

def test_free_function_resolution_override_virtual_transitive(harness):
    """multiSource/free_function_resolution_override_virtual_transitive.sol"""
    app = harness.compile_and_deploy("multiSource/free_function_resolution_override_virtual_transitive.sol")
    # g() -> 1339
    r = harness.call(app, "g()")
    assert r.abi_return == 1339

def test_free_function_transitive_import(harness):
    """multiSource/free_function_transitive_import.sol"""
    app = harness.compile_and_deploy("multiSource/free_function_transitive_import.sol")
    # i() -> 1337
    r = harness.call(app, "i()")
    assert r.abi_return == 1337

def test_import_(harness):
    """multiSource/import.sol"""
    app = harness.compile_and_deploy("multiSource/import.sol")
    # f(uint256): 1337 -> 1337
    r = harness.call(app, "f(uint256)", 1337)
    assert r.abi_return == 1337
    # g(uint256): 1337 -> 1338
    r = harness.call(app, "g(uint256)", 1337)
    assert r.abi_return == 1338

def test_import_overloaded_function(harness):
    """multiSource/import_overloaded_function.sol"""
    app = harness.compile_and_deploy("multiSource/import_overloaded_function.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2)

def test_imported_free_function_via_alias(harness):
    """multiSource/imported_free_function_via_alias.sol"""
    app = harness.compile_and_deploy("multiSource/imported_free_function_via_alias.sol")
    # g() -> 61337
    r = harness.call(app, "g()")
    assert r.abi_return == 61337

def test_imported_free_function_via_alias_direct_call(harness):
    """multiSource/imported_free_function_via_alias_direct_call.sol"""
    app = harness.compile_and_deploy("multiSource/imported_free_function_via_alias_direct_call.sol")
    # h() -> 61337
    r = harness.call(app, "h()")
    assert r.abi_return == 61337

def test_reimport_imported_function(harness):
    """multiSource/reimport_imported_function.sol"""
    app = harness.compile_and_deploy("multiSource/reimport_imported_function.sol")
    # foo() -> 1337
    r = harness.call(app, "foo()")
    assert r.abi_return == 1337
