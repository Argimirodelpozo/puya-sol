"""Tests for the using category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_calldata_memory_copy(harness):
    """using/contracts/calldata_memory_copy.sol"""
    app = harness.compile_and_deploy("using/contracts/calldata_memory_copy.sol")
    r = harness.call(app, "f(uint256[])", [1, 2, 8])
    assert as_int(r.abi_return) == 11

def test_free_function_braces(harness):
    """using/contracts/free_function_braces.sol"""
    app = harness.compile_and_deploy("using/contracts/free_function_braces.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert as_int(r.abi_return) == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert as_int(r.abi_return) == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert as_int(r.abi_return) == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert as_int(r.abi_return) == 0

def test_free_function_multi(harness):
    """using/contracts/free_function_multi.sol"""
    app = harness.compile_and_deploy("using/contracts/free_function_multi.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert as_int(r.abi_return) == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert as_int(r.abi_return) == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert as_int(r.abi_return) == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert as_int(r.abi_return) == 0

def test_free_functions_individual(harness):
    """using/contracts/free_functions_individual.sol"""
    app = harness.compile_and_deploy("using/contracts/free_functions_individual.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert as_int(r.abi_return) == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert as_int(r.abi_return) == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert as_int(r.abi_return) == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert as_int(r.abi_return) == 0

def test_imported_functions(harness):
    """using/contracts/imported_functions.sol"""
    app = harness.compile_and_deploy("using/contracts/imported_functions.sol")
    # f(uint256): 5 -> 12
    r = harness.call(app, "f(uint256)", 5)
    assert as_int(r.abi_return) == 12
    # f(uint256): 10 -> 0x16
    r = harness.call(app, "f(uint256)", 10)
    assert as_int(r.abi_return) == 22

def test_library_functions_inside_contract(harness):
    """using/contracts/library_functions_inside_contract.sol"""
    app = harness.compile_and_deploy("using/contracts/library_functions_inside_contract.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # h() -> 3
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 3

def test_library_on_interface(harness):
    """using/contracts/library_on_interface.sol"""
    app = harness.compile_and_deploy("using/contracts/library_on_interface.sol")
    # x() -> 7
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 7

def test_library_through_module(harness):
    """using/contracts/library_through_module.sol"""
    app = harness.compile_and_deploy("using/contracts/library_through_module.sol")
    # f(uint256): 5 -> 5
    r = harness.call(app, "f(uint256)", 5)
    assert as_int(r.abi_return) == 5
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert as_int(r.abi_return) == 10
    # g(uint256): 5 -> 1
    r = harness.call(app, "g(uint256)", 5)
    assert as_int(r.abi_return) == 1
    # g(uint256): 10 -> 1
    r = harness.call(app, "g(uint256)", 10)
    assert as_int(r.abi_return) == 1

def test_module_renamed(harness):
    """using/contracts/module_renamed.sol"""
    app = harness.compile_and_deploy("using/contracts/module_renamed.sol")
    # test(uint256,uint256): 1, 1 -> 9, 3
    r = harness.call(app, "test(uint256,uint256)", 1, 1)
    assert tuple(as_int(x) for x in r.abi_return) == (9, 3)

def test_private_library_function(harness):
    """using/contracts/private_library_function.sol"""
    app = harness.compile_and_deploy("using/contracts/private_library_function.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_recursive_import(harness):
    """using/contracts/recursive_import.sol"""
    app = harness.compile_and_deploy("using/contracts/recursive_import.sol")
    # f() -> 11
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 11

def test_using_global_all_the_types(harness):
    """using/contracts/using_global_all_the_types.sol"""
    app = harness.compile_and_deploy("using/contracts/using_global_all_the_types.sol")
    # f() -> 1, 7, 9
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 7, 9)

def test_using_global_for_global(harness):
    """using/contracts/using_global_for_global.sol"""
    app = harness.compile_and_deploy("using/contracts/using_global_for_global.sol")
    # f(uint256): 100 -> 111
    r = harness.call(app, "f(uint256)", 100)
    assert as_int(r.abi_return) == 111

def test_using_global_invisible(harness):
    """using/contracts/using_global_invisible.sol"""
    app = harness.compile_and_deploy("using/contracts/using_global_invisible.sol")
    # test() -> 3
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 3

def test_using_global_library(harness):
    """using/contracts/using_global_library.sol"""
    app = harness.compile_and_deploy("using/contracts/using_global_library.sol")
    # f() -> 2, 1
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 1)
