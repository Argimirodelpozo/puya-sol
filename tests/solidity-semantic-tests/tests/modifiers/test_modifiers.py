"""Tests for the modifiers category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_access_through_contract_name(harness):
    """modifiers/contracts/access_through_contract_name.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/access_through_contract_name.sol")
    # x() -> 7
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 7
    # f() -> 9
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 9
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # g() -> 0x0a
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 10
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # f() -> 9
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 9
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2

def test_access_through_module_name(harness):
    """modifiers/contracts/access_through_module_name.sol"""
    pytest.fail("`M.M.C.m` modifier access via chained module-name imports — compiler-side parse/lookup gap.")

def test_break_in_modifier(harness):
    """modifiers/contracts/break_in_modifier.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/break_in_modifier.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2

def test_continue_in_modifier(harness):
    """modifiers/contracts/continue_in_modifier.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/continue_in_modifier.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # x() -> 5
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 5

def test_evaluation_order(harness):
    """modifiers/contracts/evaluation_order.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/evaluation_order.sol")
    # query() -> 0x20, 7, 4, 2, 6, 1, 3, 5, 7
    r = harness.call(app, "query()")
    # TODO: verify structural decoding matches expected: 32, 7, 4, 2, 6, 1, 3, 5, 7
    assert not r.reverted

def test_function_modifier(harness):
    """modifiers/contracts/function_modifier.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier.sol")
    # getOne() -> 0
    r = harness.call(app, "getOne()")
    assert as_int(r.abi_return) == 0
    # getOne(), 1 wei -> 1
    r = harness.call(app, "getOne()", payment_wei=1)
    assert as_int(r.abi_return) == 1

def test_function_modifier_calling_functions_in_creation_context(harness):
    """modifiers/contracts/function_modifier_calling_functions_in_creation_context.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_calling_functions_in_creation_context.sol")
    # getData() -> 0x4300
    r = harness.call(app, "getData()")
    assert as_int(r.abi_return) == 17152

def test_function_modifier_empty(harness):
    """modifiers/contracts/function_modifier_empty.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_empty.sol")
    # f() -> false
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is False

def test_function_modifier_for_constructor(harness):
    """modifiers/contracts/function_modifier_for_constructor.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_for_constructor.sol")
    # getData() -> 6
    r = harness.call(app, "getData()")
    assert as_int(r.abi_return) == 6

def test_function_modifier_library(harness):
    """modifiers/contracts/function_modifier_library.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_library.sol")
    # f() -> 0x202
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 514

def test_function_modifier_library_inheritance(harness):
    """modifiers/contracts/function_modifier_library_inheritance.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_library_inheritance.sol")
    # f() -> 0x202
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 514

def test_function_modifier_local_variables(harness):
    """modifiers/contracts/function_modifier_local_variables.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_local_variables.sol")
    # f(bool): true -> 0
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 0
    # f(bool): false -> 3
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 3

def test_function_modifier_loop(harness):
    """modifiers/contracts/function_modifier_loop.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_loop.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 10

def test_function_modifier_loop_viair(harness):
    """modifiers/contracts/function_modifier_loop_viair.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_loop_viair.sol", via_yul_behavior=True)
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_function_modifier_multi_invocation(harness):
    """modifiers/contracts/function_modifier_multi_invocation.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_multi_invocation.sol")
    # f(bool): false -> 1
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 1
    # f(bool): true -> 2
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 2

def test_function_modifier_multi_invocation_viair(harness):
    """modifiers/contracts/function_modifier_multi_invocation_viair.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_multi_invocation_viair.sol", via_yul_behavior=True)
    # f(bool): false -> 1
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 1
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1

def test_function_modifier_multi_with_return(harness):
    """modifiers/contracts/function_modifier_multi_with_return.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_multi_with_return.sol")
    # f(bool): false -> 1
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 1
    # f(bool): true -> 2
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 2

def test_function_modifier_multiple_times(harness):
    """modifiers/contracts/function_modifier_multiple_times.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_multiple_times.sol")
    # f(uint256): 3 -> 10
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 10
    # a() -> 10
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 10

def test_function_modifier_multiple_times_local_vars(harness):
    """modifiers/contracts/function_modifier_multiple_times_local_vars.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_multiple_times_local_vars.sol")
    # f(uint256): 3 -> 10
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 10
    # a() -> 0
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0

def test_function_modifier_overriding(harness):
    """modifiers/contracts/function_modifier_overriding.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_overriding.sol")
    # f() -> false
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is False

def test_function_modifier_return_reference(harness):
    """modifiers/contracts/function_modifier_return_reference.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_modifier_return_reference.sol")
    # f() -> 2, 3
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 3)

def test_function_return_parameter(harness):
    """modifiers/contracts/function_return_parameter.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_return_parameter.sol")
    # f(uint8): 5 -> 0x00
    r = harness.call(app, "f(uint8)", 5)
    assert as_int(r.abi_return) == 0

def test_function_return_parameter_complex(harness):
    """modifiers/contracts/function_return_parameter_complex.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/function_return_parameter_complex.sol")
    # f() -> 0x10, 0x20, 0x40
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (16, 32, 64)
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # shouldFail(uint256): 1 -> FAILURE, hex"08c379a0", 0x20, 13, "a is not zero"
    r = harness.call(app, "shouldFail(uint256)", 1, expect_revert=True)
    assert r.reverted
    # shouldFail(uint256): 0 -> FAILURE, hex"08c379a0", 0x20, 13, "b is not zero"
    r = harness.call(app, "shouldFail(uint256)", 0, expect_revert=True)
    assert r.reverted
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # g() -> 5
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 5
    # x() -> 5
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 5

def test_modifer_recursive(harness):
    """modifiers/contracts/modifer_recursive.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/modifer_recursive.sol")
    # called() -> 0x00
    r = harness.call(app, "called()")
    assert as_int(r.abi_return) == 0
    # f(uint256): 5 -> 0x0100000000
    r = harness.call(app, "f(uint256)", 5)
    assert as_int(r.abi_return) == 4294967296
    # called() -> 6
    r = harness.call(app, "called()")
    assert as_int(r.abi_return) == 6

def test_modifier_in_constructor_ice(harness):
    """modifiers/contracts/modifier_in_constructor_ice.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/modifier_in_constructor_ice.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_modifier_init_return(harness):
    """modifiers/contracts/modifier_init_return.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/modifier_init_return.sol")
    # f(uint256): 9 -> 0x00, 0x00, 0x00, 0x00, 0x00
    r = harness.call(app, "f(uint256)", 9)
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0
    assert not r.reverted
    # f(uint256): 10 -> 0x00, 0x00, 3, 0x00, 0x00
    r = harness.call(app, "f(uint256)", 10)
    # TODO: verify structural decoding matches expected: 0, 0, 3, 0, 0
    assert not r.reverted

def test_modifiers_in_construction_context(harness):
    """modifiers/contracts/modifiers_in_construction_context.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/modifiers_in_construction_context.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_return_does_not_skip_modifier(harness):
    """modifiers/contracts/return_does_not_skip_modifier.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/return_does_not_skip_modifier.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2
    # x() -> 9
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 9

def test_return_in_modifier(harness):
    """modifiers/contracts/return_in_modifier.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/return_in_modifier.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # x() -> 4
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 4

def test_stacked_return_with_modifiers(harness):
    """modifiers/contracts/stacked_return_with_modifiers.sol"""
    pytest.fail("Stacked modifiers `f() public m m m` with early `return` from each — modifier inlining order specific to EVM semantics.")

def test_transient_state_variable_value_type(harness):
    """modifiers/contracts/transient_state_variable_value_type.sol"""
    app = harness.compile_and_deploy("modifiers/contracts/transient_state_variable_value_type.sol")
    # f() -> 100
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 100
