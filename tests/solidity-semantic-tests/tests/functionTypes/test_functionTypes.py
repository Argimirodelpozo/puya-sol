"""Auto-generated tests for the functionTypes category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_address_member(harness):
    """functionTypes/address_member.sol"""
    app = harness.compile_and_deploy("functionTypes/address_member.sol")
    # f() -> 0x1234, 0x1234
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (4660, 4660)

def test_call_to_zero_initialized_function_type_ir(harness):
    """functionTypes/call_to_zero_initialized_function_type_ir.sol"""
    app = harness.compile_and_deploy("functionTypes/call_to_zero_initialized_function_type_ir.sol", via_yul_behavior=True)
    # t() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_call_to_zero_initialized_function_type_legacy(harness):
    """functionTypes/call_to_zero_initialized_function_type_legacy.sol"""
    app = harness.compile_and_deploy("functionTypes/call_to_zero_initialized_function_type_legacy.sol")
    # t() -> FAILURE
    r = harness.call(app, "t()", expect_revert=True)
    assert r.reverted

def test_comparison_operator_for_external_function_cleans_dirty_bits(harness):
    """functionTypes/comparison_operator_for_external_function_cleans_dirty_bits.sol"""
    app = harness.compile_and_deploy("functionTypes/comparison_operator_for_external_function_cleans_dirty_bits.sol")
    # comparison_operators_for_external_function_pointers_with_dirty_bits() -> true
    r = harness.call(app, "comparison_operators_for_external_function_pointers_with_dirty_bits()")
    assert r.abi_return is True

def test_comparison_operators_for_external_functions(harness):
    """functionTypes/comparison_operators_for_external_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/comparison_operators_for_external_functions.sol")
    # comparison_operators_for_external_functions() -> true
    r = harness.call(app, "comparison_operators_for_external_functions()")
    assert r.abi_return is True
    # comparison_operators_for_local_external_function_pointers() -> true
    r = harness.call(app, "comparison_operators_for_local_external_function_pointers()")
    assert r.abi_return is True

def test_duplicated_function_definition_with_same_id_in_internal_dispatcher(harness):
    """functionTypes/duplicated_function_definition_with_same_id_in_internal_dispatcher.sol"""
    app = harness.compile_and_deploy("functionTypes/duplicated_function_definition_with_same_id_in_internal_dispatcher.sol")
    # f()
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type(harness):
    """functionTypes/external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type.sol"""
    app = harness.compile_and_deploy("functionTypes/external_functions_with_calldata_args_assigned_to_function_pointers_with_memory_type.sol")
    # main() -> true
    r = harness.call(app, "main()")
    assert r.abi_return is True

def test_function_delete_stack(harness):
    """functionTypes/function_delete_stack.sol"""
    app = harness.compile_and_deploy("functionTypes/function_delete_stack.sol")
    # test() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_function_delete_storage(harness):
    """functionTypes/function_delete_storage.sol"""
    app = harness.compile_and_deploy("functionTypes/function_delete_storage.sol")
    # set() -> 7
    r = harness.call(app, "set()")
    assert r.abi_return == 7
    # ca() -> 7
    r = harness.call(app, "ca()")
    assert r.abi_return == 7
    # d() -> 1
    r = harness.call(app, "d()")
    assert r.abi_return == 1
    # ca() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "ca()", expect_revert=True)
    assert r.reverted

def test_function_external_delete_storage(harness):
    """functionTypes/function_external_delete_storage.sol"""
    app = harness.compile_and_deploy("functionTypes/function_external_delete_storage.sol")
    # isF() -> false
    r = harness.call(app, "isF()")
    assert r.abi_return is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert r.abi_return is True
    # deleteFunction() ->
    r = harness.call(app, "deleteFunction()")
    # (void return — call succeeding is the assertion)
    # isF() -> false
    r = harness.call(app, "isF()")
    assert r.abi_return is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert r.abi_return is True
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # isF() -> true
    r = harness.call(app, "isF()")
    assert r.abi_return is True
    # isZero() -> false
    r = harness.call(app, "isZero()")
    assert r.abi_return is False
    # deleteFunction() ->
    r = harness.call(app, "deleteFunction()")
    # (void return — call succeeding is the assertion)
    # isF() -> false
    r = harness.call(app, "isF()")
    assert r.abi_return is False
    # isZero() -> true
    r = harness.call(app, "isZero()")
    assert r.abi_return is True

def test_function_type_library_internal(harness):
    """functionTypes/function_type_library_internal.sol"""
    app = harness.compile_and_deploy("functionTypes/function_type_library_internal.sol")
    # f(uint256[]): 0x20, 0x3, 0x1, 0x7, 0x3 -> 11
    r = harness.call(app, "f(uint256[])", 32, 3, 1, 7, 3)
    assert r.abi_return == 11

def test_inline_array_with_value_call_option(harness):
    """functionTypes/inline_array_with_value_call_option.sol"""
    app = harness.compile_and_deploy("functionTypes/inline_array_with_value_call_option.sol")
    # h(), 1 ether -> 1
    r = harness.call(app, "h()", payment_wei=1000000000000000000)
    assert r.abi_return == 1

def test_mapping_of_functions(harness):
    """functionTypes/mapping_of_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/mapping_of_functions.sol")
    # success() -> false
    r = harness.call(app, "success()")
    assert r.abi_return is False
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7
    # success() -> false
    r = harness.call(app, "success()")
    assert r.abi_return is False
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7
    # success() -> true
    r = harness.call(app, "success()")
    assert r.abi_return is True

def test_pass_function_types_externally(harness):
    """functionTypes/pass_function_types_externally.sol"""
    app = harness.compile_and_deploy("functionTypes/pass_function_types_externally.sol")
    # f(uint256): 7 -> 8
    r = harness.call(app, "f(uint256)", 7)
    assert r.abi_return == 8
    # f2(uint256): 7 -> 8
    r = harness.call(app, "f2(uint256)", 7)
    assert r.abi_return == 8

def test_pass_function_types_internally(harness):
    """functionTypes/pass_function_types_internally.sol"""
    app = harness.compile_and_deploy("functionTypes/pass_function_types_internally.sol")
    # f(uint256): 7 -> 8
    r = harness.call(app, "f(uint256)", 7)
    assert r.abi_return == 8

def test_same_function_in_construction_and_runtime(harness):
    """functionTypes/same_function_in_construction_and_runtime.sol"""
    app = harness.compile_and_deploy("functionTypes/same_function_in_construction_and_runtime.sol")
    # runtime(uint256): 3 -> 6
    r = harness.call(app, "runtime(uint256)", 3)
    assert r.abi_return == 6
    # initial() -> 4
    r = harness.call(app, "initial()")
    assert r.abi_return == 4

def test_same_function_in_construction_and_runtime_equality_check(harness):
    """functionTypes/same_function_in_construction_and_runtime_equality_check.sol"""
    app = harness.compile_and_deploy("functionTypes/same_function_in_construction_and_runtime_equality_check.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_selector_1(harness):
    """functionTypes/selector_1.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_1.sol", via_yul_behavior=True)
    # test() -> 0xcf9f23b500000000000000000000000000000000000000000000000000000000, 0x7defb41000000000000000000000000000000000000000000000000000000000, 0xcf9f23b500000000000000000000000000000000000000000000000000000000, 0x7defb41000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (93909934780908389925680208513171772481190584319973189686494540973005321797632, 56962625267091901377327903097178401288657785329182381021111551275490816819200, 93909934780908389925680208513171772481190584319973189686494540973005321797632, 56962625267091901377327903097178401288657785329182381021111551275490816819200)

def test_selector_2(harness):
    """functionTypes/selector_2.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_2.sol", via_yul_behavior=True)
    # test() -> 0xcf9f23b500000000000000000000000000000000000000000000000000000000, 0x7defb41000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (93909934780908389925680208513171772481190584319973189686494540973005321797632, 56962625267091901377327903097178401288657785329182381021111551275490816819200)

def test_selector_assignment_expression(harness):
    """functionTypes/selector_assignment_expression.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_assignment_expression.sol")
    # f()
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # z() -> true
    r = harness.call(app, "z()")
    assert r.abi_return is True

def test_selector_expression_side_effect(harness):
    """functionTypes/selector_expression_side_effect.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_expression_side_effect.sol")
    # f() -> 42
    r = harness.call(app, "f()")
    assert r.abi_return == 42

def test_selector_ternary(harness):
    """functionTypes/selector_ternary.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_ternary.sol")
    # h(bool): true -> 0x26121ff000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(bool)", True)
    assert r.abi_return == 17219911917854084299749778639755835327755045716242581057573779540915269926912
    # h(bool): false -> 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "h(bool)", False)
    assert r.abi_return == 102264414861304285884729579275374176073311626045629144087797787832582884294656

def test_selector_ternary_function_pointer_from_function_call(harness):
    """functionTypes/selector_ternary_function_pointer_from_function_call.sol"""
    app = harness.compile_and_deploy("functionTypes/selector_ternary_function_pointer_from_function_call.sol")
    # test(bool): true -> 0x26121ff000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "test(bool)", True)
    assert r.abi_return == 17219911917854084299749778639755835327755045716242581057573779540915269926912
    # test(bool): false -> 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "test(bool)", False)
    assert r.abi_return == 102264414861304285884729579275374176073311626045629144087797787832582884294656

def test_stack_height_check_on_adding_gas_variable_to_function(harness):
    """functionTypes/stack_height_check_on_adding_gas_variable_to_function.sol"""
    app = harness.compile_and_deploy("functionTypes/stack_height_check_on_adding_gas_variable_to_function.sol")
    # test_function() -> true
    r = harness.call(app, "test_function()")
    assert r.abi_return is True

def test_store_function(harness):
    """functionTypes/store_function.sol"""
    app = harness.compile_and_deploy("functionTypes/store_function.sol")
    # t() -> 9
    r = harness.call(app, "t()")
    assert r.abi_return == 9

def test_struct_with_external_function(harness):
    """functionTypes/struct_with_external_function.sol"""
    app = harness.compile_and_deploy("functionTypes/struct_with_external_function.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2)

def test_struct_with_functions(harness):
    """functionTypes/struct_with_functions.sol"""
    app = harness.compile_and_deploy("functionTypes/struct_with_functions.sol")
    # f() -> 1, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2)

def test_ternary_contract_internal_function(harness):
    """functionTypes/ternary_contract_internal_function.sol"""
    app = harness.compile_and_deploy("functionTypes/ternary_contract_internal_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert r.abi_return == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert r.abi_return == 2

def test_ternary_contract_library_internal_function(harness):
    """functionTypes/ternary_contract_library_internal_function.sol"""
    app = harness.compile_and_deploy("functionTypes/ternary_contract_library_internal_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert r.abi_return == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert r.abi_return == 2

def test_ternary_contract_public_function(harness):
    """functionTypes/ternary_contract_public_function.sol"""
    app = harness.compile_and_deploy("functionTypes/ternary_contract_public_function.sol")
    # test(bool): true -> 1
    r = harness.call(app, "test(bool)", True)
    assert r.abi_return == 1
    # test(bool): false -> 2
    r = harness.call(app, "test(bool)", False)
    assert r.abi_return == 2

def test_uninitialized_internal_storage_function_call(harness):
    """functionTypes/uninitialized_internal_storage_function_call.sol"""
    app = harness.compile_and_deploy("functionTypes/uninitialized_internal_storage_function_call.sol")
    # f() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
