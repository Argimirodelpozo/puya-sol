"""Tests for the storageLayoutSpecifier category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_base_slot_max_value(harness):
    """storageLayoutSpecifier/contracts/base_slot_max_value.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/base_slot_max_value.sol")
    # f(uint256): 4 -> 8
    r = harness.call(app, "f(uint256)", 4)
    assert as_int(r.abi_return) == 8

def test_constructor(harness):
    """storageLayoutSpecifier/contracts/constructor.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/constructor.sol", ctor_args=[1, 2, 3])
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # y() -> 4
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 4
    # z() -> 6
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 6

def test_delete(harness):
    """storageLayoutSpecifier/contracts/delete.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/delete.sol")
    # fillArray() -> 3
    r = harness.call(app, "fillArray()")
    assert as_int(r.abi_return) == 3
    # arrayLength() -> 3
    r = harness.call(app, "arrayLength()")
    assert as_int(r.abi_return) == 3
    # array(uint256): 2 -> 3
    r = harness.call(app, "array(uint256)", 2)
    assert as_int(r.abi_return) == 3
    # deleteLast() ->
    r = harness.call(app, "deleteLast()")
    # (void return — call succeeding is the assertion)
    # array(uint256): 2 -> 0
    r = harness.call(app, "array(uint256)", 2)
    assert as_int(r.abi_return) == 0
    # deleteArray() ->
    r = harness.call(app, "deleteArray()")
    # (void return — call succeeding is the assertion)
    # arrayLength() -> 0
    r = harness.call(app, "arrayLength()")
    assert as_int(r.abi_return) == 0

def test_delete_transient_storage(harness):
    """storageLayoutSpecifier/contracts/delete_transient_storage.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/delete_transient_storage.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_dynamic_array_storage_end(harness):
    """storageLayoutSpecifier/contracts/dynamic_array_storage_end.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/dynamic_array_storage_end.sol")
    # init() ->
    r = harness.call(app, "init()")
    # (void return — call succeeding is the assertion)
    # validate() ->
    r = harness.call(app, "validate()")
    # (void return — call succeeding is the assertion)
    # clear() ->
    r = harness.call(app, "clear()")
    # (void return — call succeeding is the assertion)

def test_function_from_base_contract(harness):
    """storageLayoutSpecifier/contracts/function_from_base_contract.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/function_from_base_contract.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # test() -> 6
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 6
    # x() -> 2
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 2
    # y() -> 4
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 4
    # z() -> 6
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 6

def test_getters(harness):
    """storageLayoutSpecifier/contracts/getters.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/getters.sol")
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # y() -> 2
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 2
    # z() -> 3
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 3

def test_inheritance_from_abstract_contract(harness):
    """storageLayoutSpecifier/contracts/inheritance_from_abstract_contract.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inheritance_from_abstract_contract.sol")
    # f() -> 10
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 10
    # x() -> 8
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 8
    # y() -> 10
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 10

def test_inheritance_from_interface(harness):
    """storageLayoutSpecifier/contracts/inheritance_from_interface.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inheritance_from_interface.sol")
    # f(uint256): 8 -> 6
    r = harness.call(app, "f(uint256)", 8)
    assert as_int(r.abi_return) == 6
    # y() -> 6
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 6

def test_inheritance_from_same_base_state_var_slots(harness):
    """storageLayoutSpecifier/contracts/inheritance_from_same_base_state_var_slots.sol"""
    # Test ctor deploys 3 child contracts (new A/B/C), each app-create + fund
    # payment = 6 inner txns at postInit.
    app = harness.compile_and_deploy('storageLayoutSpecifier/contracts/inheritance_from_same_base_state_var_slots.sol',
                                     postinit_inner_txns=6)
    r = harness.call(app, 'contractASlots()')
    assert as_int(r.abi_return) == 0
    r = harness.call(app, 'contractBSlots()')
    assert tuple(as_int(x) for x in r.abi_return) == (5, 6,)
    r = harness.call(app, 'contractCSlots()')
    assert tuple(as_int(x) for x in r.abi_return) == (9, 10,)

def test_inheritance_simple(harness):
    """storageLayoutSpecifier/contracts/inheritance_simple.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inheritance_simple.sol")
    # f() -> 8
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 8
    # x() -> 6
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 6
    # y() -> 8
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 8

def test_inheritance_state_variable_slot_offset(harness):
    """storageLayoutSpecifier/contracts/inheritance_state_variable_slot_offset.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inheritance_state_variable_slot_offset.sol")
    # xSlotOffset() -> 7, 0
    r = harness.call(app, "xSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 0)
    # ySlotOffset() -> 8, 0
    r = harness.call(app, "ySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 0)
    # wSlotOffset() -> 8, 16
    r = harness.call(app, "wSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 16)
    # zSlotOffset() -> 9, 0
    r = harness.call(app, "zSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (9, 0)

def test_inline_assembly_direct_load(harness):
    """storageLayoutSpecifier/contracts/inline_assembly_direct_load.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inline_assembly_direct_load.sol")
    # f() -> 16
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 16
    # x() -> 16
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 16

def test_inline_assembly_direct_store(harness):
    """storageLayoutSpecifier/contracts/inline_assembly_direct_store.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/inline_assembly_direct_store.sol")
    # f() -> 16
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 16
    # x() -> 16
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 16

def test_last_allowed_storage_slot(harness):
    """storageLayoutSpecifier/contracts/last_allowed_storage_slot.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/last_allowed_storage_slot.sol")
    # f(uint256): 4 -> 8
    r = harness.call(app, "f(uint256)", 4)
    assert as_int(r.abi_return) == 8
    # x() -> 8
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 8

def test_mapping_storage_end(harness):
    """storageLayoutSpecifier/contracts/mapping_storage_end.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/mapping_storage_end.sol")
    # init() ->
    r = harness.call(app, "init()")
    # (void return — call succeeding is the assertion)
    # validate() ->
    r = harness.call(app, "validate()")
    # (void return — call succeeding is the assertion)

def test_multiple_inheritance(harness):
    """storageLayoutSpecifier/contracts/multiple_inheritance.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/multiple_inheritance.sol")
    # test() -> 1, 2, 3, 5
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 5)
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # y() -> 2
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 2
    # w() -> 3
    r = harness.call(app, "w()")
    assert as_int(r.abi_return) == 3
    # z() -> 5
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 5

def test_multiple_inheritance_state_var_slots(harness):
    """storageLayoutSpecifier/contracts/multiple_inheritance_state_var_slots.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/multiple_inheritance_state_var_slots.sol")
    # xSlotOffset() -> 2, 0
    r = harness.call(app, "xSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 0)
    # ySlotOffset() -> 3, 0
    r = harness.call(app, "ySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 0)
    # wSlotOffset() -> 3, 4
    r = harness.call(app, "wSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 4)
    # zSlotOffset() -> 4, 0
    r = harness.call(app, "zSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (4, 0)

def test_state_variable_arithmetic_expression(harness):
    """storageLayoutSpecifier/contracts/state_variable_arithmetic_expression.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_arithmetic_expression.sol")
    # f(uint256): 2 -> 0
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 0
    # f(uint256): 3 -> 5
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 5
    # f(uint256): 5 -> 15
    r = harness.call(app, "f(uint256)", 5)
    assert as_int(r.abi_return) == 15
    # x() -> 10
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 10
    # y() -> 17
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 17
    # z() -> 15
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 15

def test_state_variable_constant_and_immutable(harness):
    """storageLayoutSpecifier/contracts/state_variable_constant_and_immutable.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_constant_and_immutable.sol")
    # f() -> 11
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 11
    # g() -> 200
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 200

def test_state_variable_dynamic_array(harness):
    """storageLayoutSpecifier/contracts/state_variable_dynamic_array.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_dynamic_array.sol")
    # initA() -> 1, 2, 3
    r = harness.call(app, "initA()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3)
    # arrayA(uint256): 0 -> 1
    r = harness.call(app, "arrayA(uint256)", 0)
    assert as_int(r.abi_return) == 1
    # arrayALength() -> 3
    r = harness.call(app, "arrayALength()")
    assert as_int(r.abi_return) == 3
    # arrayCLength() -> 0
    r = harness.call(app, "arrayCLength()")
    assert as_int(r.abi_return) == 0
    # initCFromAInReverse() -> 3, 2, 1
    r = harness.call(app, "initCFromAInReverse()")
    assert tuple(as_int(x) for x in r.abi_return) == (3, 2, 1)
    # clearA() ->
    r = harness.call(app, "clearA()")
    # (void return — call succeeding is the assertion)
    # arrayC(uint256): 0 -> 3
    r = harness.call(app, "arrayC(uint256)", 0)
    assert as_int(r.abi_return) == 3
    # arrayALength() -> 0
    r = harness.call(app, "arrayALength()")
    assert as_int(r.abi_return) == 0
    # arrayCLength() -> 3
    r = harness.call(app, "arrayCLength()")
    assert as_int(r.abi_return) == 3

def test_state_variable_enum(harness):
    """storageLayoutSpecifier/contracts/state_variable_enum.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_enum.sol")
    # cSlotOffset() -> 42, 0
    r = harness.call(app, "cSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0)
    # checkBlue() -> false
    r = harness.call(app, "checkBlue()")
    assert bool(as_int(r.abi_return)) is False
    # setBlue() ->
    r = harness.call(app, "setBlue()")
    # (void return — call succeeding is the assertion)
    # checkBlue() -> true
    r = harness.call(app, "checkBlue()")
    assert bool(as_int(r.abi_return)) is True

def test_state_variable_mapping(harness):
    """storageLayoutSpecifier/contracts/state_variable_mapping.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_mapping.sol")
    # setup() ->
    r = harness.call(app, "setup()")
    # (void return — call succeeding is the assertion)
    # open(uint256): 3 -> 0x20, 5, "Empty"
    r = harness.call(app, "open(uint256)", 3)
    assert r.abi_return == 'Empty'
    # open(uint256): 2 -> 0x20, 6, "Locked"
    r = harness.call(app, "open(uint256)", 2)
    assert r.abi_return == 'Locked'
    # open(uint256): 1 -> 0x20, 7, "Monster"
    r = harness.call(app, "open(uint256)", 1)
    assert r.abi_return == 'Monster'

def test_state_variable_reference_types_slot_offset(harness):
    """storageLayoutSpecifier/contracts/state_variable_reference_types_slot_offset.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_reference_types_slot_offset.sol")
    # s1SlotOffset() -> 42, 0
    r = harness.call(app, "s1SlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0)
    # dArraySlotOffset() -> 44, 0
    r = harness.call(app, "dArraySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (44, 0)
    # sArraySlotOffset() -> 49, 0
    r = harness.call(app, "sArraySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (49, 0)
    # bArraySlotOffset() -> 69, 0
    r = harness.call(app, "bArraySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (69, 0)
    # strSlotOffset() -> 70, 0
    r = harness.call(app, "strSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (70, 0)

def test_state_variable_slot_offset(harness):
    """storageLayoutSpecifier/contracts/state_variable_slot_offset.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_slot_offset.sol")
    # xSlotOffset() -> 7, 0
    r = harness.call(app, "xSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 0)
    # ySlotOffset() -> 7, 1
    r = harness.call(app, "ySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 1)
    # zSlotOffset() -> 8, 0
    r = harness.call(app, "zSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 0)

def test_state_variable_struct(harness):
    """storageLayoutSpecifier/contracts/state_variable_struct.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variable_struct.sol")
    # initS1() -> 7, 0x0abc, 1
    r = harness.call(app, "initS1()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 2748, 1)
    # initS2() -> 8, 0x0def, 0
    r = harness.call(app, "initS2()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 3567, 0)
    # s1() -> 7, 0x0abc, 1
    r = harness.call(app, "s1()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 2748, 1)
    # s2() -> 8, 0x0def, 0
    r = harness.call(app, "s2()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 3567, 0)

def test_state_variables_transient(harness):
    """storageLayoutSpecifier/contracts/state_variables_transient.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/state_variables_transient.sol")
    # test() -> 2
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 2

def test_storage_reference_array(harness):
    """storageLayoutSpecifier/contracts/storage_reference_array.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/storage_reference_array.sol")
    # initUsingReference() ->
    r = harness.call(app, "initUsingReference()")
    # (void return — call succeeding is the assertion)
    # array(uint256): 0 -> 1
    r = harness.call(app, "array(uint256)", 0)
    assert as_int(r.abi_return) == 1
    # array(uint256): 9 -> 10
    r = harness.call(app, "array(uint256)", 9)
    assert as_int(r.abi_return) == 10

def test_storage_reference_inheritance(harness):
    """storageLayoutSpecifier/contracts/storage_reference_inheritance.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/storage_reference_inheritance.sol")
    # InitUsingReference() ->
    r = harness.call(app, "InitUsingReference()")
    # (void return — call succeeding is the assertion)
    # s() -> 2, true
    r = harness.call(app, "s()")
    # TODO: verify expected: 2 | true
    assert not r.reverted

def test_storage_reference_library_function(harness):
    """storageLayoutSpecifier/contracts/storage_reference_library_function.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/storage_reference_library_function.sol")
    # initUsingReference() ->
    r = harness.call(app, "initUsingReference()")
    # (void return — call succeeding is the assertion)
    # s() -> 2, true
    r = harness.call(app, "s()")
    # TODO: verify expected: 2 | true
    assert not r.reverted

def test_transient_state_variable_slot_offset(harness):
    """storageLayoutSpecifier/contracts/transient_state_variable_slot_offset.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/transient_state_variable_slot_offset.sol")
    # xSlotOffset() -> 0, 0
    r = harness.call(app, "xSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # ySlotOffset() -> 7, 0
    r = harness.call(app, "ySlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 0)
    # wSlotOffset() -> 1, 0
    r = harness.call(app, "wSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)
    # zSlotOffset() -> 8, 0
    r = harness.call(app, "zSlotOffset()")
    assert tuple(as_int(x) for x in r.abi_return) == (8, 0)

def test_variable_cleanup(harness):
    """storageLayoutSpecifier/contracts/variable_cleanup.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/variable_cleanup.sol")
    # f(uint256,int256,bytes3): 0x0100, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f, "abc" -> 0x00, 0x7f, "ab"
    r = harness.call(app, "f(uint256,int256,bytes3)", 256, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f, bytes.fromhex('616263'))
    # TODO: verify expected: 0x00 | 0x7f | "ab"
    assert not r.reverted

def test_variable_cleanup_sstore(harness):
    """storageLayoutSpecifier/contracts/variable_cleanup_sstore.sol"""
    app = harness.compile_and_deploy('storageLayoutSpecifier/contracts/variable_cleanup_sstore.sol')
    r = harness.call(app, 'f()')

def test_virtual_functions(harness):
    """storageLayoutSpecifier/contracts/virtual_functions.sol"""
    app = harness.compile_and_deploy("storageLayoutSpecifier/contracts/virtual_functions.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 3
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3
    # h() -> 6
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 6
    # x() -> 1
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 1
    # y() -> 3
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 3
    # z() -> 6
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 6
