"""Tests for the inheritance category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_access_base_storage(harness):
    """inheritance/contracts/access_base_storage.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/access_base_storage.sol")
    # setData(uint256,uint256): 1, 2 -> true
    r = harness.call(app, "setData(uint256,uint256)", 1, 2)
    assert bool(as_int(r.abi_return)) is True
    # getViaBase() -> 1
    r = harness.call(app, "getViaBase()")
    assert as_int(r.abi_return) == 1
    # getViaDerived() -> 1, 2
    r = harness.call(app, "getViaDerived()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_address_overload_resolution(harness):
    """inheritance/contracts/address_overload_resolution.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/address_overload_resolution.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 5
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 5

def test_base_access_to_function_type_variables(harness):
    """inheritance/contracts/base_access_to_function_type_variables.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/base_access_to_function_type_variables.sol")
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # h() -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "h()", expect_revert=True)
    assert r.reverted
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # h() -> 2
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 2

def test_constructor_inheritance_init_order(harness):
    """inheritance/contracts/constructor_inheritance_init_order.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_inheritance_init_order.sol", via_yul_behavior=True)
    # y() -> 42
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 42

def test_constructor_inheritance_init_order_2(harness):
    """inheritance/contracts/constructor_inheritance_init_order_2.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_inheritance_init_order_2.sol")
    # y() -> 42
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 42

def test_constructor_inheritance_init_order_3_legacy(harness):
    """inheritance/contracts/constructor_inheritance_init_order_3_legacy.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_inheritance_init_order_3_legacy.sol")
    # x() -> 4
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 4

def test_constructor_inheritance_init_order_3_viaIR(harness):
    """inheritance/contracts/constructor_inheritance_init_order_3_viaIR.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_inheritance_init_order_3_viaIR.sol", via_yul_behavior=True)
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2

def test_constructor_with_params(harness):
    """inheritance/contracts/constructor_with_params.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_with_params.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 2
    # k() -> 0
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) == 0

def test_constructor_with_params_diamond_inheritance(harness):
    """inheritance/contracts/constructor_with_params_diamond_inheritance.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_with_params_diamond_inheritance.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 2
    # j() -> 2
    r = harness.call(app, "j()")
    assert as_int(r.abi_return) == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) == 1

def test_constructor_with_params_inheritance(harness):
    """inheritance/contracts/constructor_with_params_inheritance.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_with_params_inheritance.sol", ctor_args=[2, 0])
    # i() -> 2
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) == 1

def test_constructor_with_params_inheritance_2(harness):
    """inheritance/contracts/constructor_with_params_inheritance_2.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/constructor_with_params_inheritance_2.sol")
    # i() -> 2
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 2
    # k() -> 1
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) == 1

def test_derived_overload_base_function_direct(harness):
    """inheritance/contracts/derived_overload_base_function_direct.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/derived_overload_base_function_direct.sol")
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2

def test_derived_overload_base_function_indirect(harness):
    """inheritance/contracts/derived_overload_base_function_indirect.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/derived_overload_base_function_indirect.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 10
    # h() -> 2
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 2

def test_explicit_base_class(harness):
    """inheritance/contracts/explicit_base_class.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/explicit_base_class.sol")
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_inherited_constant_state_var(harness):
    """inheritance/contracts/inherited_constant_state_var.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_constant_state_var.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7

def test_inherited_function(harness):
    """inheritance/contracts/inherited_function.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1

def test_inherited_function_calldata_calldata_interface(harness):
    """inheritance/contracts/inherited_function_calldata_calldata_interface.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function_calldata_calldata_interface.sol")
    # g() -> 42
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 42

def test_inherited_function_calldata_memory(harness):
    """inheritance/contracts/inherited_function_calldata_memory.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function_calldata_memory.sol")
    # g() -> 23
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 23

def test_inherited_function_calldata_memory_interface(harness):
    """inheritance/contracts/inherited_function_calldata_memory_interface.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function_calldata_memory_interface.sol")
    # g() -> 42
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 42

def test_inherited_function_from_a_library(harness):
    """inheritance/contracts/inherited_function_from_a_library.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function_from_a_library.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1

def test_inherited_function_through_dispatch(harness):
    """inheritance/contracts/inherited_function_through_dispatch.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/inherited_function_through_dispatch.sol")
    # g() -> 1
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1

def test_interface_inheritance_conversions(harness):
    """inheritance/contracts/interface_inheritance_conversions.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/interface_inheritance_conversions.sol")
    # convertParent() -> 1
    r = harness.call(app, "convertParent()")
    assert as_int(r.abi_return) == 1
    # convertSubA() -> 1, 2
    r = harness.call(app, "convertSubA()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # convertSubB() -> 1, 3
    r = harness.call(app, "convertSubB()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3)

def test_member_notation_ctor(harness):
    """inheritance/contracts/member_notation_ctor.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/member_notation_ctor.sol")
    # g(int256): -1 -> -1
    r = harness.call(app, "g(int256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # g(int256): 10 -> 10
    r = harness.call(app, "g(int256)", 10)
    assert as_int(r.abi_return) == 10

def test_overloaded_function_call_resolve_to_first(harness):
    """inheritance/contracts/overloaded_function_call_resolve_to_first.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/overloaded_function_call_resolve_to_first.sol")
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3

def test_overloaded_function_call_resolve_to_second(harness):
    """inheritance/contracts/overloaded_function_call_resolve_to_second.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/overloaded_function_call_resolve_to_second.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 10

def test_overloaded_function_call_with_if_else(harness):
    """inheritance/contracts/overloaded_function_call_with_if_else.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/overloaded_function_call_with_if_else.sol")
    # g(bool): true -> 3
    r = harness.call(app, "g(bool)", True)
    assert as_int(r.abi_return) == 3
    # g(bool): false -> 10
    r = harness.call(app, "g(bool)", False)
    assert as_int(r.abi_return) == 10

def test_pass_dynamic_arguments_to_the_base(harness):
    """inheritance/contracts/pass_dynamic_arguments_to_the_base.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/pass_dynamic_arguments_to_the_base.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert as_int(r.abi_return) == 4

def test_pass_dynamic_arguments_to_the_base_base(harness):
    """inheritance/contracts/pass_dynamic_arguments_to_the_base_base.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/pass_dynamic_arguments_to_the_base_base.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert as_int(r.abi_return) == 4

def test_pass_dynamic_arguments_to_the_base_base_with_gap(harness):
    """inheritance/contracts/pass_dynamic_arguments_to_the_base_base_with_gap.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/pass_dynamic_arguments_to_the_base_base_with_gap.sol")
    # m_i() -> 4
    r = harness.call(app, "m_i()")
    assert as_int(r.abi_return) == 4

def test_state_variables_init_order(harness):
    """inheritance/contracts/state_variables_init_order.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/state_variables_init_order.sol")
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1

def test_state_variables_init_order_2(harness):
    """inheritance/contracts/state_variables_init_order_2.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/state_variables_init_order_2.sol")
    # z() -> 1
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 1

def test_state_variables_init_order_3(harness):
    """inheritance/contracts/state_variables_init_order_3.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/state_variables_init_order_3.sol", via_yul_behavior=True)
    # a() -> 17
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 17
    # b() -> 42
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 42
    # c() -> 51
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 51
    # b_a() -> 17
    r = harness.call(app, "b_a()")
    assert as_int(r.abi_return) == 17
    # b_b() -> 42
    r = harness.call(app, "b_b()")
    assert as_int(r.abi_return) == 42
    # b_c() -> 51
    r = harness.call(app, "b_c()")
    assert as_int(r.abi_return) == 51
    # d() -> 23
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 23
    # e() -> 42
    r = harness.call(app, "e()")
    assert as_int(r.abi_return) == 42

def test_super_in_constructor(harness):
    """inheritance/contracts/super_in_constructor.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/super_in_constructor.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 15

def test_super_in_constructor_assignment(harness):
    """inheritance/contracts/super_in_constructor_assignment.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/super_in_constructor_assignment.sol")
    # f() -> 15
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 15

def test_super_overload(harness):
    """inheritance/contracts/super_overload.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/super_overload.sol")
    # g() -> 10
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 10
    # h() -> 2
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 2

def test_transient_storage_state_variable(harness):
    """inheritance/contracts/transient_storage_state_variable.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/transient_storage_state_variable.sol")
    # f() -> 1, 2, 3, 4
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 4)

def test_transient_storage_state_variable_abstract_contract(harness):
    """inheritance/contracts/transient_storage_state_variable_abstract_contract.sol"""
    app = harness.compile_and_deploy("inheritance/contracts/transient_storage_state_variable_abstract_contract.sol")
    # f() -> 1, 1, 2, 2
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 1, 2, 2)

def test_value_for_constructor(harness):
    """inheritance/contracts/value_for_constructor.sol"""
    app = harness.compile_and_deploy(
        "inheritance/contracts/value_for_constructor.sol", fund_wei=22,
    )
    assert harness.call(app, "getFlag()").abi_return is True
    assert bytes(harness.call(app, "getName()").abi_return) == b"abc"
    # Main is funded with 22 wei; the ctor forwards 10 to the child Helper.
    # Subtract the Main app's MBR baseline so we compare just the wei.
    r = harness.call(app, "getBalances()")
    me, them = as_int(r.abi_return[0]), as_int(r.abi_return[1])
    assert me - app.balance_baseline == 12
    # The child app's MBR is implementation-defined; assert the 10 wei the
    # ctor forwarded is at least included in its balance.
    assert them >= 10
