"""Tests for the functionTypes category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_address_member(harness):
    """functionTypes/contracts/address_member.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/address_member.sol")
    # f() -> 0x1234, 0x1234
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (4660, 4660)

def test_call_to_zero_initialized_function_type_ir(harness):
    """functionTypes/contracts/call_to_zero_initialized_function_type_ir.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/call_to_zero_initialized_function_type_ir.sol", via_yul_behavior=True)
    # t() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_call_to_zero_initialized_function_type_legacy(harness):
    """functionTypes/contracts/call_to_zero_initialized_function_type_legacy.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/call_to_zero_initialized_function_type_legacy.sol")
    # t() -> FAILURE
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_comparison_operator_for_external_function_cleans_dirty_bits(harness):
    """functionTypes/contracts/comparison_operator_for_external_function_cleans_dirty_bits.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/comparison_operator_for_external_function_cleans_dirty_bits.sol")
    # comparison_operators_for_external_function_pointers_with_dirty_bits() -> true
    r = harness.call(app, "comparison_operators_for_external_function_pointers_with_dirty_bits()")
    assert bool(as_int(r.abi_return)) is True

def test_comparison_operators_for_external_functions(harness):
    """functionTypes/contracts/comparison_operators_for_external_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/comparison_operators_for_external_functions.sol")
    # comparison_operators_for_external_functions() -> true
    r = harness.call(app, "comparison_operators_for_external_functions()")
    assert bool(as_int(r.abi_return)) is True
    # comparison_operators_for_local_external_function_pointers() -> true
    r = harness.call(app, "comparison_operators_for_local_external_function_pointers()")
    assert bool(as_int(r.abi_return)) is True

def test_duplicated_function_definition_with_same_id_in_internal_dispatcher(harness):
    """functionTypes/contracts/duplicated_function_definition_with_same_id_in_internal_dispatcher.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/duplicated_function_definition_with_same_id_in_internal_dispatcher.sol")
    # f()
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type(harness):
    """functionTypes/contracts/external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type.sol")
    # main() -> true
    r = harness.call(app, "main()")
    assert bool(as_int(r.abi_return)) is True

def test_function_delete_stack(harness):
    """functionTypes/contracts/function_delete_stack.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/function_delete_stack.sol")
    # test() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_function_delete_storage(harness):
    """functionTypes/contracts/function_delete_storage.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/function_delete_storage.sol")
    # set() -> 7
    r = harness.call(app, "set()")
    assert as_int(r.abi_return) == 7
    # ca() -> 7
    r = harness.call(app, "ca()")
    assert as_int(r.abi_return) == 7
    # d() -> 1
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 1
    # ca() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "ca()", expect_revert=True)
    assert r.reverted

def test_function_external_delete_storage(harness):
    """functionTypes/contracts/function_external_delete_storage.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/function_external_delete_storage.sol")
    # isF() -> false
    r = harness.call(app, "isF()")
    assert bool(as_int(r.abi_return)) is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert bool(as_int(r.abi_return)) is True
    # deleteFunction() ->
    r = harness.call(app, "deleteFunction()")
    # (void return — call succeeding is the assertion)
    # isF() -> false
    r = harness.call(app, "isF()")
    assert bool(as_int(r.abi_return)) is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert bool(as_int(r.abi_return)) is True
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # isF() -> true
    r = harness.call(app, "isF()")
    assert bool(as_int(r.abi_return)) is True
    # isZero() -> false
    r = harness.call(app, "isZero()")
    assert bool(as_int(r.abi_return)) is False
    # deleteFunction() ->
    r = harness.call(app, "deleteFunction()")
    # (void return — call succeeding is the assertion)
    # isF() -> false
    r = harness.call(app, "isF()")
    assert bool(as_int(r.abi_return)) is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert bool(as_int(r.abi_return)) is True

def test_function_type_library_internal(harness):
    """functionTypes/contracts/function_type_library_internal.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/function_type_library_internal.sol")
    r = harness.call(app, "f(uint256[])", [1, 7, 3])
    assert as_int(r.abi_return) == 11

def test_inline_array_with_value_call_option(harness):
    """functionTypes/contracts/inline_array_with_value_call_option.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/inline_array_with_value_call_option.sol")
    # 1 ether = 10^18 microalgos overflows; 10k microalgos is enough.
    r = harness.call(app, "h()", payment_wei=10_000)
    assert as_int(r.abi_return) == 1

def test_mapping_of_functions(harness):
    """functionTypes/contracts/mapping_of_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/mapping_of_functions.sol")
    # success() -> false
    r = harness.call(app, "success()")
    assert bool(as_int(r.abi_return)) is False
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7
    # success() -> false
    r = harness.call(app, "success()")
    assert bool(as_int(r.abi_return)) is False
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7
    # success() -> true
    r = harness.call(app, "success()")
    assert bool(as_int(r.abi_return)) is True

def test_pass_function_types_externally(harness):
    """functionTypes/contracts/pass_function_types_externally.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/pass_function_types_externally.sol")
    # f(uint256): 7 -> 8
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 8
    # f2(uint256): 7 -> 8
    r = harness.call(app, "f2(uint256)", 7)
    assert as_int(r.abi_return) == 8

def test_pass_function_types_internally(harness):
    """functionTypes/contracts/pass_function_types_internally.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/pass_function_types_internally.sol")
    # f(uint256): 7 -> 8
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 8

def test_same_function_in_construction_and_runtime(harness):
    """functionTypes/contracts/same_function_in_construction_and_runtime.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/same_function_in_construction_and_runtime.sol")
    # runtime(uint256): 3 -> 6
    r = harness.call(app, "runtime(uint256)", 3)
    assert as_int(r.abi_return) == 6
    # initial() -> 4
    r = harness.call(app, "initial()")
    assert as_int(r.abi_return) == 4

def test_same_function_in_construction_and_runtime_equality_check(harness):
    """functionTypes/contracts/same_function_in_construction_and_runtime_equality_check.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/same_function_in_construction_and_runtime_equality_check.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_selector_1(harness):
    """functionTypes/contracts/selector_1.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_1.sol", via_yul_behavior=True)
    # EVM_DIVERGENCE: selectors are sha512_256 on AVM (keccak on EVM).
    from framework import arc4_selector
    s1 = arc4_selector("ext()")
    s2 = arc4_selector("pub()")
    r = harness.call(app, "test()")
    assert [bytes(x) for x in r.abi_return] == [s1, s2, s1, s2]


def test_selector_2(harness):
    """functionTypes/contracts/selector_2.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_2.sol", via_yul_behavior=True)
    r = harness.call(app, "test()")
    # EVM_DIVERGENCE: sha512_256 selectors.
    from framework import arc4_selector
    assert [bytes(x) for x in r.abi_return] == [arc4_selector("ext()"), arc4_selector("pub()")]

def test_selector_assignment_expression(harness):
    """functionTypes/contracts/selector_assignment_expression.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_assignment_expression.sol")
    # f()
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # z() -> true
    r = harness.call(app, "z()")
    assert bool(as_int(r.abi_return)) is True

def test_selector_expression_side_effect(harness):
    """functionTypes/contracts/selector_expression_side_effect.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_expression_side_effect.sol")
    # f() -> 42
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 42

def test_selector_ternary(harness):
    """functionTypes/contracts/selector_ternary.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_ternary.sol")
    # EVM_DIVERGENCE: sha512_256 selectors.
    from framework import arc4_selector
    assert bytes(harness.call(app, "h(bool)", True).abi_return) == arc4_selector("f()")
    assert bytes(harness.call(app, "h(bool)", False).abi_return) == arc4_selector("g()")

def test_selector_ternary_function_pointer_from_function_call(harness):
    """functionTypes/contracts/selector_ternary_function_pointer_from_function_call.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/selector_ternary_function_pointer_from_function_call.sol")
    # EVM_DIVERGENCE: sha512_256 selectors.
    from framework import arc4_selector
    assert bytes(harness.call(app, "test(bool)", True).abi_return) == arc4_selector("f()")
    assert bytes(harness.call(app, "test(bool)", False).abi_return) == arc4_selector("g()")

def test_stack_height_check_on_adding_gas_variable_to_function(harness):
    """functionTypes/contracts/stack_height_check_on_adding_gas_variable_to_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/stack_height_check_on_adding_gas_variable_to_function.sol")
    # test_function() -> true
    r = harness.call(app, "test_function()")
    assert bool(as_int(r.abi_return)) is True

def test_store_function(harness):
    """functionTypes/contracts/store_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/store_function.sol")
    # t() -> 9
    r = harness.call(app, "t()")
    assert as_int(r.abi_return) == 9

def test_struct_with_external_function(harness):
    """functionTypes/contracts/struct_with_external_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/struct_with_external_function.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_struct_with_functions(harness):
    """functionTypes/contracts/struct_with_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/struct_with_functions.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_ternary_contract_internal_function(harness):
    """functionTypes/contracts/ternary_contract_internal_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/ternary_contract_internal_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert as_int(r.abi_return) == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert as_int(r.abi_return) == 2

def test_ternary_contract_library_internal_function(harness):
    """functionTypes/contracts/ternary_contract_library_internal_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/ternary_contract_library_internal_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert as_int(r.abi_return) == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert as_int(r.abi_return) == 2

def test_ternary_contract_public_function(harness):
    """functionTypes/contracts/ternary_contract_public_function.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/ternary_contract_public_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert as_int(r.abi_return) == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert as_int(r.abi_return) == 2

def test_uninitialized_internal_storage_function_call(harness):
    """functionTypes/contracts/uninitialized_internal_storage_function_call.sol"""
    app = harness.compile_and_deploy("functionTypes/contracts/uninitialized_internal_storage_function_call.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
