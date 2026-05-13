"""Auto-generated tests for the using category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_calldata_memory_copy(harness):
    """using/calldata_memory_copy.sol"""
    app = harness.compile_and_deploy("using/calldata_memory_copy.sol")
    # f(uint256[]): 0x20, 3, 1, 2, 8 -> 11
    r = harness.call(app, "f(uint256[])", 32, 3, 1, 2, 8)
    assert r.abi_return == 11

def test_free_function_braces(harness):
    """using/free_function_braces.sol"""
    app = harness.compile_and_deploy("using/free_function_braces.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert r.abi_return == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert r.abi_return == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert r.abi_return == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert r.abi_return == 0

def test_free_function_multi(harness):
    """using/free_function_multi.sol"""
    app = harness.compile_and_deploy("using/free_function_multi.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert r.abi_return == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert r.abi_return == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert r.abi_return == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert r.abi_return == 0

def test_free_functions_individual(harness):
    """using/free_functions_individual.sol"""
    app = harness.compile_and_deploy("using/free_functions_individual.sol")
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert r.abi_return == 10
    # g(uint256): 10 -> 0
    r = harness.call(app, "g(uint256)", 10)
    assert r.abi_return == 0
    # f(uint256): 256 -> 0x0100
    r = harness.call(app, "f(uint256)", 256)
    assert r.abi_return == 256
    # g(uint256): 256 -> 0
    r = harness.call(app, "g(uint256)", 256)
    assert r.abi_return == 0

def test_imported_functions(harness):
    """using/imported_functions.sol"""
    app = harness.compile_and_deploy("using/imported_functions.sol")
    # f(uint256): 5 -> 12
    r = harness.call(app, "f(uint256)", 5)
    assert r.abi_return == 12
    # f(uint256): 10 -> 0x16
    r = harness.call(app, "f(uint256)", 10)
    assert r.abi_return == 22

def test_library_functions_inside_contract(harness):
    """using/library_functions_inside_contract.sol"""
    app = harness.compile_and_deploy("using/library_functions_inside_contract.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert r.abi_return == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2
    # h() -> 3
    r = harness.call(app, "h()")
    assert r.abi_return == 3

def test_library_on_interface(harness):
    """using/library_on_interface.sol"""
    app = harness.compile_and_deploy("using/library_on_interface.sol")
    # x() -> 7
    r = harness.call(app, "x()")
    assert r.abi_return == 7

def test_library_through_module(harness):
    """using/library_through_module.sol"""
    app = harness.compile_and_deploy("using/library_through_module.sol")
    # f(uint256): 5 -> 5
    r = harness.call(app, "f(uint256)", 5)
    assert r.abi_return == 5
    # f(uint256): 10 -> 10
    r = harness.call(app, "f(uint256)", 10)
    assert r.abi_return == 10
    # g(uint256): 5 -> 1
    r = harness.call(app, "g(uint256)", 5)
    assert r.abi_return == 1
    # g(uint256): 10 -> 1
    r = harness.call(app, "g(uint256)", 10)
    assert r.abi_return == 1

def test_module_renamed(harness):
    """using/module_renamed.sol"""
    app = harness.compile_and_deploy("using/module_renamed.sol")
    # test(uint256,uint256): 1, 1 -> 9, 3
    r = harness.call(app, "test(uint256,uint256)", 1, 1)
    assert tuple(r.abi_return) == (9, 3)

def test_private_library_function(harness):
    """using/private_library_function.sol"""
    app = harness.compile_and_deploy("using/private_library_function.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert r.abi_return == 2

def test_recursive_import(harness):
    """using/recursive_import.sol"""
    app = harness.compile_and_deploy("using/recursive_import.sol")
    # f() -> 11
    r = harness.call(app, "f()")
    assert r.abi_return == 11

def test_using_global_all_the_types(harness):
    """using/using_global_all_the_types.sol"""
    app = harness.compile_and_deploy("using/using_global_all_the_types.sol")
    # f() -> 1, 7, 9
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 7, 9)

def test_using_global_for_global(harness):
    """using/using_global_for_global.sol"""
    app = harness.compile_and_deploy("using/using_global_for_global.sol")
    # f(uint256): 100 -> 111
    r = harness.call(app, "f(uint256)", 100)
    assert r.abi_return == 111

def test_using_global_invisible(harness):
    """using/using_global_invisible.sol"""
    app = harness.compile_and_deploy("using/using_global_invisible.sol")
    # test() -> 3
    r = harness.call(app, "test()")
    assert r.abi_return == 3

def test_using_global_library(harness):
    """using/using_global_library.sol"""
    app = harness.compile_and_deploy("using/using_global_library.sol")
    # f() -> 2, 1
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (2, 1)
