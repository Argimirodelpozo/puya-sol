"""Tests for the multiSource category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_circular_import(harness):
    """multiSource/contracts/circular_import.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/circular_import.sol")
    # foo() -> 1
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 1

def test_circular_import_2(harness):
    """multiSource/contracts/circular_import_2.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/circular_import_2.sol")
    # foo() -> 992
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 992

def test_circular_reimport(harness):
    """multiSource/contracts/circular_reimport.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/circular_reimport.sol")
    # foo() -> 0x60
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 96

def test_circular_reimport_2(harness):
    """multiSource/contracts/circular_reimport_2.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/circular_reimport_2.sol")
    # foo() -> 0x2324
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 8996

def test_free_different_interger_types(harness):
    """multiSource/contracts/free_different_interger_types.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_different_interger_types.sol")
    # foo() -> 24, true
    r = harness.call(app, "foo()")
    # TODO: verify expected: 24 | true
    assert not r.reverted

def test_free_function_resolution_base_contract(harness):
    """multiSource/contracts/free_function_resolution_base_contract.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_function_resolution_base_contract.sol")
    # h() -> 1337
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 1337

def test_free_function_resolution_override_virtual(harness):
    """multiSource/contracts/free_function_resolution_override_virtual.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_function_resolution_override_virtual.sol")
    # g() -> 1337
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1337

def test_free_function_resolution_override_virtual_super(harness):
    """multiSource/contracts/free_function_resolution_override_virtual_super.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_function_resolution_override_virtual_super.sol")
    # g() -> 1337
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1337

def test_free_function_resolution_override_virtual_transitive(harness):
    """multiSource/contracts/free_function_resolution_override_virtual_transitive.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_function_resolution_override_virtual_transitive.sol")
    # g() -> 1339
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1339

def test_free_function_transitive_import(harness):
    """multiSource/contracts/free_function_transitive_import.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/free_function_transitive_import.sol")
    # i() -> 1337
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 1337

def test_import_(harness):
    """multiSource/contracts/import.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/import.sol")
    # f(uint256): 1337 -> 1337
    r = harness.call(app, "f(uint256)", 1337)
    assert as_int(r.abi_return) == 1337
    # g(uint256): 1337 -> 1338
    r = harness.call(app, "g(uint256)", 1337)
    assert as_int(r.abi_return) == 1338

def test_import_overloaded_function(harness):
    """multiSource/contracts/import_overloaded_function.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/import_overloaded_function.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_imported_free_function_via_alias(harness):
    """multiSource/contracts/imported_free_function_via_alias.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/imported_free_function_via_alias.sol")
    # g() -> 61337
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 61337

def test_imported_free_function_via_alias_direct_call(harness):
    """multiSource/contracts/imported_free_function_via_alias_direct_call.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/imported_free_function_via_alias_direct_call.sol")
    # h() -> 61337
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 61337

def test_reimport_imported_function(harness):
    """multiSource/contracts/reimport_imported_function.sol"""
    app = harness.compile_and_deploy("multiSource/contracts/reimport_imported_function.sol")
    # foo() -> 1337
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 1337
