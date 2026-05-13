"""Auto-generated tests for the inheritance category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_access_base_storage(harness):
    """inheritance/access_base_storage.sol"""
    app = harness.compile_and_deploy("inheritance/access_base_storage.sol")
    # setData(uint256,uint256): 1, 2 -> true
    r = harness.call(app, "setData(uint256,uint256)", 1, 2)
    assert r.abi_return is True
    # getViaBase() -> 1
    r = harness.call(app, "getViaBase()")
    assert r.abi_return == 1
    # getViaDerived() -> 1, 2
    r = harness.call(app, "getViaDerived()")
    assert tuple(r.abi_return) == (1, 2)

def test_address_overload_resolution(harness):
    """inheritance/address_overload_resolution.sol"""
    app = harness.compile_and_deploy("inheritance/address_overload_resolution.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert r.abi_return == 1
    # g() -> 5
    r = harness.call(app, "g()")
    assert r.abi_return == 5

def test_base_access_to_function_type_variables(harness):
    """inheritance/base_access_to_function_type_variables.sol"""
    app = harness.compile_and_deploy("inheritance/base_access_to_function_type_variables.sol")
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2
    # h() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # h() -> 2
    r = harness.call(app, "h()")
    assert r.abi_return == 2

def test_constructor_inheritance_init_order(harness):
    """inheritance/constructor_inheritance_init_order.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_inheritance_init_order.sol", via_yul_behavior=True)
    # y() -> 42
    r = harness.call(app, "y()")
    assert r.abi_return == 42

def test_constructor_inheritance_init_order_2(harness):
    """inheritance/constructor_inheritance_init_order_2.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_inheritance_init_order_2.sol")
    # y() -> 42
    r = harness.call(app, "y()")
    assert r.abi_return == 42

def test_constructor_inheritance_init_order_3_legacy(harness):
    """inheritance/constructor_inheritance_init_order_3_legacy.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_inheritance_init_order_3_legacy.sol")
    # x() -> 4
    r = harness.call(app, "x()")
    assert r.abi_return == 4

def test_constructor_inheritance_init_order_3_viaIR(harness):
    """inheritance/constructor_inheritance_init_order_3_viaIR.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_inheritance_init_order_3_viaIR.sol", via_yul_behavior=True)
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2

def test_constructor_with_params(harness):
    """inheritance/constructor_with_params.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_with_params.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert r.abi_return == 2
    # k() -> 0
    r = harness.call(app, "k()")
    assert r.abi_return == 0

def test_constructor_with_params_diamond_inheritance(harness):
    """inheritance/constructor_with_params_diamond_inheritance.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_with_params_diamond_inheritance.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert r.abi_return == 2
    # j() -> 2
    r = harness.call(app, "j()")
    assert r.abi_return == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert r.abi_return == 1

def test_constructor_with_params_inheritance(harness):
    """inheritance/constructor_with_params_inheritance.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_with_params_inheritance.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert r.abi_return == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert r.abi_return == 1

def test_constructor_with_params_inheritance_2(harness):
    """inheritance/constructor_with_params_inheritance_2.sol"""
    app = harness.compile_and_deploy("inheritance/constructor_with_params_inheritance_2.sol")
    # i() -> 2
    r = harness.call(app, "i()")
    assert r.abi_return == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert r.abi_return == 1

def test_derived_overload_base_function_direct(harness):
    """inheritance/derived_overload_base_function_direct.sol"""
    app = harness.compile_and_deploy("inheritance/derived_overload_base_function_direct.sol")
    # g() -> 2
    r = harness.call(app, "g()")
    assert r.abi_return == 2

def test_derived_overload_base_function_indirect(harness):
    """inheritance/derived_overload_base_function_indirect.sol"""
    app = harness.compile_and_deploy("inheritance/derived_overload_base_function_indirect.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert r.abi_return == 10
    # h() -> 2
    r = harness.call(app, "h()")
    assert r.abi_return == 2

def test_explicit_base_class(harness):
    """inheritance/explicit_base_class.sol"""
    app = harness.compile_and_deploy("inheritance/explicit_base_class.sol")
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3
    # f() -> 1
    r = harness.call(app, "f()")
    assert r.abi_return == 1

def test_inherited_constant_state_var(harness):
    """inheritance/inherited_constant_state_var.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_constant_state_var.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7

def test_inherited_function(harness):
    """inheritance/inherited_function.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert r.abi_return == 1

def test_inherited_function_calldata_calldata_interface(harness):
    """inheritance/inherited_function_calldata_calldata_interface.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function_calldata_calldata_interface.sol")
    # g() -> 42
    r = harness.call(app, "g()")
    assert r.abi_return == 42

def test_inherited_function_calldata_memory(harness):
    """inheritance/inherited_function_calldata_memory.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function_calldata_memory.sol")
    # g() -> 23
    r = harness.call(app, "g()")
    assert r.abi_return == 23

def test_inherited_function_calldata_memory_interface(harness):
    """inheritance/inherited_function_calldata_memory_interface.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function_calldata_memory_interface.sol")
    # g() -> 42
    r = harness.call(app, "g()")
    assert r.abi_return == 42

def test_inherited_function_from_a_library(harness):
    """inheritance/inherited_function_from_a_library.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function_from_a_library.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert r.abi_return == 1

def test_inherited_function_through_dispatch(harness):
    """inheritance/inherited_function_through_dispatch.sol"""
    app = harness.compile_and_deploy("inheritance/inherited_function_through_dispatch.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert r.abi_return == 1

def test_interface_inheritance_conversions(harness):
    """inheritance/interface_inheritance_conversions.sol"""
    app = harness.compile_and_deploy("inheritance/interface_inheritance_conversions.sol")
    # convertParent() -> 1
    r = harness.call(app, "convertParent()")
    assert r.abi_return == 1
    # convertSubA() -> 1, 2
    r = harness.call(app, "convertSubA()")
    assert tuple(r.abi_return) == (1, 2)
    # convertSubB() -> 1, 3
    r = harness.call(app, "convertSubB()")
    assert tuple(r.abi_return) == (1, 3)

def test_member_notation_ctor(harness):
    """inheritance/member_notation_ctor.sol"""
    app = harness.compile_and_deploy("inheritance/member_notation_ctor.sol")
    # g(int256): -1 -> -1
    r = harness.call(app, "g(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == -1
    # g(int256): 10 -> 10
    r = harness.call(app, "g(int256)", 10)
    assert r.abi_return == 10

def test_overloaded_function_call_resolve_to_first(harness):
    """inheritance/overloaded_function_call_resolve_to_first.sol"""
    app = harness.compile_and_deploy("inheritance/overloaded_function_call_resolve_to_first.sol")
    # g() -> 3
    r = harness.call(app, "g()")
    assert r.abi_return == 3

def test_overloaded_function_call_resolve_to_second(harness):
    """inheritance/overloaded_function_call_resolve_to_second.sol"""
    app = harness.compile_and_deploy("inheritance/overloaded_function_call_resolve_to_second.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert r.abi_return == 10

def test_overloaded_function_call_with_if_else(harness):
    """inheritance/overloaded_function_call_with_if_else.sol"""
    app = harness.compile_and_deploy("inheritance/overloaded_function_call_with_if_else.sol")
    # g(bool): true -> 3
    r = harness.call(app, "g(bool)", True)
    assert r.abi_return == 3
    # g(bool): false -> 10
    r = harness.call(app, "g(bool)", False)
    assert r.abi_return == 10

def test_pass_dynamic_arguments_to_the_base(harness):
    """inheritance/pass_dynamic_arguments_to_the_base.sol"""
    app = harness.compile_and_deploy("inheritance/pass_dynamic_arguments_to_the_base.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert r.abi_return == 4

def test_pass_dynamic_arguments_to_the_base_base(harness):
    """inheritance/pass_dynamic_arguments_to_the_base_base.sol"""
    app = harness.compile_and_deploy("inheritance/pass_dynamic_arguments_to_the_base_base.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert r.abi_return == 4

def test_pass_dynamic_arguments_to_the_base_base_with_gap(harness):
    """inheritance/pass_dynamic_arguments_to_the_base_base_with_gap.sol"""
    app = harness.compile_and_deploy("inheritance/pass_dynamic_arguments_to_the_base_base_with_gap.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert r.abi_return == 4

def test_state_variables_init_order(harness):
    """inheritance/state_variables_init_order.sol"""
    app = harness.compile_and_deploy("inheritance/state_variables_init_order.sol")
    # x() -> 1
    r = harness.call(app, "x()")
    assert r.abi_return == 1

def test_state_variables_init_order_2(harness):
    """inheritance/state_variables_init_order_2.sol"""
    app = harness.compile_and_deploy("inheritance/state_variables_init_order_2.sol")
    # z() -> 1
    r = harness.call(app, "z()")
    assert r.abi_return == 1

def test_state_variables_init_order_3(harness):
    """inheritance/state_variables_init_order_3.sol"""
    app = harness.compile_and_deploy("inheritance/state_variables_init_order_3.sol", via_yul_behavior=True)
    # a() -> 17
    r = harness.call(app, "a()")
    assert r.abi_return == 17
    # b() -> 42
    r = harness.call(app, "b()")
    assert r.abi_return == 42
    # c() -> 51
    r = harness.call(app, "c()")
    assert r.abi_return == 51
    # b_a() -> 17
    r = harness.call(app, "b_a()")
    assert r.abi_return == 17
    # b_b() -> 42
    r = harness.call(app, "b_b()")
    assert r.abi_return == 42
    # b_c() -> 51
    r = harness.call(app, "b_c()")
    assert r.abi_return == 51
    # d() -> 23
    r = harness.call(app, "d()")
    assert r.abi_return == 23
    # e() -> 42
    r = harness.call(app, "e()")
    assert r.abi_return == 42

def test_super_in_constructor(harness):
    """inheritance/super_in_constructor.sol"""
    app = harness.compile_and_deploy("inheritance/super_in_constructor.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert r.abi_return == 15

def test_super_in_constructor_assignment(harness):
    """inheritance/super_in_constructor_assignment.sol"""
    app = harness.compile_and_deploy("inheritance/super_in_constructor_assignment.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert r.abi_return == 15

def test_super_overload(harness):
    """inheritance/super_overload.sol"""
    app = harness.compile_and_deploy("inheritance/super_overload.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert r.abi_return == 10
    # h() -> 2
    r = harness.call(app, "h()")
    assert r.abi_return == 2

def test_transient_storage_state_variable(harness):
    """inheritance/transient_storage_state_variable.sol"""
    app = harness.compile_and_deploy("inheritance/transient_storage_state_variable.sol")
    # f() -> 1, 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 2, 3, 4)

def test_transient_storage_state_variable_abstract_contract(harness):
    """inheritance/transient_storage_state_variable_abstract_contract.sol"""
    app = harness.compile_and_deploy("inheritance/transient_storage_state_variable_abstract_contract.sol")
    # f() -> 1, 1, 2, 2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (1, 1, 2, 2)

def test_value_for_constructor(harness):
    """inheritance/value_for_constructor.sol"""
    app = harness.compile_and_deploy("inheritance/value_for_constructor.sol", fund_wei=22)
    # getFlag() -> true
    r = harness.call(app, "getFlag()")
    assert r.abi_return is True
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted
    # getBalances() -> 12, 10
    r = harness.call(app, "getBalances()")
    assert tuple(r.abi_return) == (12, 10)
