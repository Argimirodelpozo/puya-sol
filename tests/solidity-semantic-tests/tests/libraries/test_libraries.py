"""Tests for the libraries category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_attached_internal_library_function_accepting_calldata(harness):
    """libraries/contracts/attached_internal_library_function_accepting_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_internal_library_function_accepting_calldata.sol")
    # Returns two bytes32 values, each = first char "a" right-padded.
    expected = b"a".ljust(32, b"\x00")
    r = harness.call(app, "f(bytes)", b"abcd")
    assert [bytes(x) for x in r.abi_return] == [expected, expected]

def test_attached_internal_library_function_returning_calldata(harness):
    """libraries/contracts/attached_internal_library_function_returning_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_internal_library_function_returning_calldata.sol")
    # f(bytes): 0x20, 4, "abcd" -> 0x6100000000000000000000000000000000000000000000000000000000000000, 0x6100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes)", 'abcd')
    assert tuple(as_int(x) for x in r.abi_return) == (43874346312576839672212443538448152585028080127215369968075725190498334277632, 43874346312576839672212443538448152585028080127215369968075725190498334277632)

def test_attached_public_library_function_accepting_calldata_sol(harness):
    """libraries/contracts/attached_public_library_function_accepting_calldata.sol.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_public_library_function_accepting_calldata.sol.sol")
    # f(bytes): 0x20, 4, "abcd" -> 0x6100000000000000000000000000000000000000000000000000000000000000, 0x6100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes)", 'abcd')
    assert tuple(as_int(x) for x in r.abi_return) == (43874346312576839672212443538448152585028080127215369968075725190498334277632, 43874346312576839672212443538448152585028080127215369968075725190498334277632)

def test_attached_public_library_function_returning_calldata(harness):
    """libraries/contracts/attached_public_library_function_returning_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_public_library_function_returning_calldata.sol")
    # f(bytes): 0x20, 4, "abcd" -> 0x6100000000000000000000000000000000000000000000000000000000000000, 0x6100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes)", 'abcd')
    assert tuple(as_int(x) for x in r.abi_return) == (43874346312576839672212443538448152585028080127215369968075725190498334277632, 43874346312576839672212443538448152585028080127215369968075725190498334277632)

def test_external_call_with_function_pointer_parameter(harness):
    """libraries/contracts/external_call_with_function_pointer_parameter.sol"""
    app = harness.compile_and_deploy("libraries/contracts/external_call_with_function_pointer_parameter.sol")
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16

def test_external_call_with_storage_array_parameter(harness):
    """libraries/contracts/external_call_with_storage_array_parameter.sol"""
    app = harness.compile_and_deploy("libraries/contracts/external_call_with_storage_array_parameter.sol")
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16

def test_external_call_with_storage_mapping_parameter(harness):
    """libraries/contracts/external_call_with_storage_mapping_parameter.sol"""
    app = harness.compile_and_deploy("libraries/contracts/external_call_with_storage_mapping_parameter.sol")
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16

def test_internal_call_attached_with_parentheses(harness):
    """libraries/contracts/internal_call_attached_with_parentheses.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_call_attached_with_parentheses.sol")
    # f() -> 0x0a
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 10

def test_internal_call_unattached_with_parentheses(harness):
    """libraries/contracts/internal_call_unattached_with_parentheses.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_call_unattached_with_parentheses.sol")
    # foo() -> 3
    r = harness.call(app, "foo()")
    assert as_int(r.abi_return) == 3

def test_internal_library_function(harness):
    """libraries/contracts/internal_library_function.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_internal_library_function_attached_to_address(harness):
    """libraries/contracts/internal_library_function_attached_to_address.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_address.sol")
    # foo(address,address): 0x111122223333444455556666777788889999aAaa, 0x111122223333444455556666777788889999aAaa -> true
    r = harness.call(app, "foo(address,address)", 0x111122223333444455556666777788889999aaaa, 0x111122223333444455556666777788889999aaaa)
    assert bool(as_int(r.abi_return)) is True
    # foo(address,address): 0x111122223333444455556666777788889999aAaa, 0x0000000000000000000000000000000000000000 -> false
    r = harness.call(app, "foo(address,address)", 0x111122223333444455556666777788889999aaaa, 0)
    assert bool(as_int(r.abi_return)) is False

def test_internal_library_function_attached_to_address_named_send_transfer(harness):
    """libraries/contracts/internal_library_function_attached_to_address_named_send_transfer.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_address_named_send_transfer.sol")
    # useTransfer(address): 0x111122223333444455556666777788889999aAaa ->
    r = harness.call(app, "useTransfer(address)", 0x111122223333444455556666777788889999aaaa)
    # (void return — call succeeding is the assertion)
    # useSend(address): 0x111122223333444455556666777788889999aAaa ->
    r = harness.call(app, "useSend(address)", 0x111122223333444455556666777788889999aaaa)
    # (void return — call succeeding is the assertion)

def test_internal_library_function_attached_to_array_named_pop_push(harness):
    """libraries/contracts/internal_library_function_attached_to_array_named_pop_push.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_array_named_pop_push.sol")
    # test() ->
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)

def test_internal_library_function_attached_to_bool(harness):
    """libraries/contracts/internal_library_function_attached_to_bool.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_bool.sol")
    # foo(bool,bool): true, true -> false
    r = harness.call(app, "foo(bool,bool)", True, True)
    assert bool(as_int(r.abi_return)) is False
    # foo(bool,bool): true, false -> true
    r = harness.call(app, "foo(bool,bool)", True, False)
    assert bool(as_int(r.abi_return)) is True
    # foo(bool,bool): false, true -> true
    r = harness.call(app, "foo(bool,bool)", False, True)
    assert bool(as_int(r.abi_return)) is True
    # foo(bool,bool): false, false -> false
    r = harness.call(app, "foo(bool,bool)", False, False)
    assert bool(as_int(r.abi_return)) is False

def test_internal_library_function_attached_to_contract(harness):
    """libraries/contracts/internal_library_function_attached_to_contract.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_contract.sol")
    # test() -> 42
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 42

def test_internal_library_function_attached_to_dynamic_array(harness):
    """libraries/contracts/internal_library_function_attached_to_dynamic_array.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_dynamic_array.sol")
    # secondItem() -> 0x22
    r = harness.call(app, "secondItem()")
    assert as_int(r.abi_return) == 34

def test_internal_library_function_attached_to_enum(harness):
    """libraries/contracts/internal_library_function_attached_to_enum.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_enum.sol")
    # equalsA(uint256): 0 -> true
    r = harness.call(app, "equalsA(uint256)", 0)
    assert bool(as_int(r.abi_return)) is True
    # equalsA(uint256): 1 -> false
    r = harness.call(app, "equalsA(uint256)", 1)
    assert bool(as_int(r.abi_return)) is False

def test_internal_library_function_attached_to_external_function_type(harness):
    """libraries/contracts/internal_library_function_attached_to_external_function_type.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_external_function_type.sol")
    # test(uint256): 5 -> 10
    r = harness.call(app, "test(uint256)", 5)
    assert as_int(r.abi_return) == 10

def test_internal_library_function_attached_to_fixed_array(harness):
    """libraries/contracts/internal_library_function_attached_to_fixed_array.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_fixed_array.sol")
    # secondItem() -> 0x22
    r = harness.call(app, "secondItem()")
    assert as_int(r.abi_return) == 34

def test_internal_library_function_attached_to_fixed_bytes(harness):
    """libraries/contracts/internal_library_function_attached_to_fixed_bytes.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_fixed_bytes.sol")
    # sum(bytes2,bytes2): left(0x1100), left(0x0022) -> left(0x1122)
    r = harness.call(app, "sum(bytes2,bytes2)", 0x1100000000000000000000000000000000000000000000000000000000000000, 0x22000000000000000000000000000000000000000000000000000000000000)
    # TODO: verify expected: left(0x1122)
    assert not r.reverted

def test_internal_library_function_attached_to_integer(harness):
    """libraries/contracts/internal_library_function_attached_to_integer.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_integer.sol")
    # foo(uint256,uint256): 8, 42 -> 50
    r = harness.call(app, "foo(uint256,uint256)", 8, 42)
    assert as_int(r.abi_return) == 50

def test_internal_library_function_attached_to_interface(harness):
    """libraries/contracts/internal_library_function_attached_to_interface.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_interface.sol")
    # test() -> 42
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 42

def test_internal_library_function_attached_to_internal_function_type(harness):
    """libraries/contracts/internal_library_function_attached_to_internal_function_type.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_internal_function_type.sol")
    # test(uint256): 5 -> 10
    r = harness.call(app, "test(uint256)", 5)
    assert as_int(r.abi_return) == 10

def test_internal_library_function_attached_to_internal_function_type_named_selector(harness):
    """libraries/contracts/internal_library_function_attached_to_internal_function_type_named_selector.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_internal_function_type_named_selector.sol")
    # test(uint256): 5 -> 10
    r = harness.call(app, "test(uint256)", 5)
    assert as_int(r.abi_return) == 10

def test_internal_library_function_attached_to_literal(harness):
    """libraries/contracts/internal_library_function_attached_to_literal.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_literal.sol")
    # double42() -> 84
    r = harness.call(app, "double42()")
    assert as_int(r.abi_return) == 84
    # doubleABC() -> 0x20, 6, "abcabc"
    r = harness.call(app, "doubleABC()")
    assert r.abi_return == 'abcabc'

def test_internal_library_function_attached_to_mapping(harness):
    """libraries/contracts/internal_library_function_attached_to_mapping.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_mapping.sol")
    # mapValue(uint256): 42 -> 0x24
    r = harness.call(app, "mapValue(uint256)", 42)
    assert as_int(r.abi_return) == 36

def test_internal_library_function_attached_to_string_accepting_memory(harness):
    """libraries/contracts/internal_library_function_attached_to_string_accepting_memory.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_string_accepting_memory.sol")
    # secondChar() -> 98
    r = harness.call(app, "secondChar()")
    assert as_int(r.abi_return) == 98

def test_internal_library_function_attached_to_string_accepting_storage(harness):
    """libraries/contracts/internal_library_function_attached_to_string_accepting_storage.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_string_accepting_storage.sol")
    # test(string): 0x20, 3, "def" -> 0x40, 0x80, 3, "def", 3, "def"
    r = harness.call(app, "test(string)", 'def')
    # TODO: verify expected: 0x40 | 0x80 | 3 | "def" | 3 | "def"
    assert not r.reverted

def test_internal_library_function_attached_to_struct(harness):
    """libraries/contracts/internal_library_function_attached_to_struct.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_struct.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_internal_library_function_calling_private(harness):
    """libraries/contracts/internal_library_function_calling_private.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_calling_private.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_internal_library_function_pointer(harness):
    """libraries/contracts/internal_library_function_pointer.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_pointer.sol")
    # g() -> 66
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 66

def test_internal_library_function_return_var_size(harness):
    """libraries/contracts/internal_library_function_return_var_size.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_return_var_size.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_internal_types_in_library(harness):
    """libraries/contracts/internal_types_in_library.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_types_in_library.sol")
    # f() -> 4, 0x11
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (4, 17)

def test_library_address(harness):
    """libraries/contracts/library_address.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_address.sol")
    # addr() -> false
    r = harness.call(app, "addr()")
    assert bool(as_int(r.abi_return)) is False
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 4
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # h(uint256): 1 -> 1
    r = harness.call(app, "h(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # h(uint256): 2 -> 4
    r = harness.call(app, "h(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # h(uint256): 4 -> 16
    r = harness.call(app, "h(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # i(uint256): 1 -> 1
    r = harness.call(app, "i(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # i(uint256): 2 -> 4
    r = harness.call(app, "i(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # i(uint256): 4 -> 16
    r = harness.call(app, "i(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # j(uint256): 1 -> 1
    r = harness.call(app, "j(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # j(uint256): 2 -> 4
    r = harness.call(app, "j(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # j(uint256): 4 -> 16
    r = harness.call(app, "j(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # k(uint256): 1 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 1, expect_revert=True)
    assert r.reverted
    # k(uint256): 2 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 2, expect_revert=True)
    assert r.reverted
    # k(uint256): 4 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 4, expect_revert=True)
    assert r.reverted

def test_library_address_homestead(harness):
    """libraries/contracts/library_address_homestead.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_address_homestead.sol")
    # g(uint256,uint256): 1, 1 -> true
    r = harness.call(app, "g(uint256,uint256)", 1, 1)
    assert bool(as_int(r.abi_return)) is True
    # g(uint256,uint256): 1, 2 -> false
    r = harness.call(app, "g(uint256,uint256)", 1, 2)
    assert bool(as_int(r.abi_return)) is False
    # g(uint256,uint256): 2, 3 -> false
    r = harness.call(app, "g(uint256,uint256)", 2, 3)
    assert bool(as_int(r.abi_return)) is False
    # g(uint256,uint256): 2, 4 -> true
    r = harness.call(app, "g(uint256,uint256)", 2, 4)
    assert bool(as_int(r.abi_return)) is True
    # g(uint256,uint256): 2, 5 -> false
    r = harness.call(app, "g(uint256,uint256)", 2, 5)
    assert bool(as_int(r.abi_return)) is False
    # g(uint256,uint256): 4, 15 -> false
    r = harness.call(app, "g(uint256,uint256)", 4, 15)
    assert bool(as_int(r.abi_return)) is False
    # g(uint256,uint256): 4, 16 -> true
    r = harness.call(app, "g(uint256,uint256)", 4, 16)
    assert bool(as_int(r.abi_return)) is True
    # g(uint256,uint256): 4, 17 -> false
    r = harness.call(app, "g(uint256,uint256)", 4, 17)
    assert bool(as_int(r.abi_return)) is False

def test_library_address_via_module(harness):
    """libraries/contracts/library_address_via_module.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_address_via_module.sol")
    # addr() -> false
    r = harness.call(app, "addr()")
    assert bool(as_int(r.abi_return)) is False
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 4
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # h(uint256): 1 -> 1
    r = harness.call(app, "h(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # h(uint256): 2 -> 4
    r = harness.call(app, "h(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # h(uint256): 4 -> 16
    r = harness.call(app, "h(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # i(uint256): 1 -> 1
    r = harness.call(app, "i(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # i(uint256): 2 -> 4
    r = harness.call(app, "i(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # i(uint256): 4 -> 16
    r = harness.call(app, "i(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # j(uint256): 1 -> 1
    r = harness.call(app, "j(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # j(uint256): 2 -> 4
    r = harness.call(app, "j(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # j(uint256): 4 -> 16
    r = harness.call(app, "j(uint256)", 4)
    assert as_int(r.abi_return) == 16
    # k(uint256): 1 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 1, expect_revert=True)
    assert r.reverted
    # k(uint256): 2 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 2, expect_revert=True)
    assert r.reverted
    # k(uint256): 4 -> FAILURE, hex"4e487b71", 0x01
    r = harness.call(app, "k(uint256)", 4, expect_revert=True)
    assert r.reverted

def test_library_call_in_homestead(harness):
    """libraries/contracts/library_call_in_homestead.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_call_in_homestead.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # sender() -> 0x1212121212121212121212121212120000000012
    r = harness.call(app, "sender()")
    assert as_int(r.abi_return) == 103164821458651970696730694074090566015747358738

def test_library_delegatecall_guard_pure(harness):
    """libraries/contracts/library_delegatecall_guard_pure.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_delegatecall_guard_pure.sol")
    # f() -> 23
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 23
    # g() -> true, 23
    r = harness.call(app, "g()")
    # TODO: verify expected: true | 23
    assert not r.reverted
    # h() -> true, 23
    r = harness.call(app, "h()")
    # TODO: verify expected: true | 23
    assert not r.reverted

def test_library_delegatecall_guard_view_needed(harness):
    """libraries/contracts/library_delegatecall_guard_view_needed.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_delegatecall_guard_view_needed.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> true, 1
    r = harness.call(app, "g()")
    # TODO: verify expected: true | 1
    assert not r.reverted
    # h() -> true, 0 # this is bad - this should fail! #
    r = harness.call(app, "h()")
    # TODO: verify expected: true | 0 # this is bad - this should fail! #
    assert not r.reverted

def test_library_delegatecall_guard_view_not_needed(harness):
    """libraries/contracts/library_delegatecall_guard_view_not_needed.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_delegatecall_guard_view_not_needed.sol")
    # f() -> 84
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 84
    # g() -> true, 84
    r = harness.call(app, "g()")
    # TODO: verify expected: true | 84
    assert not r.reverted
    # h() -> true, 84
    r = harness.call(app, "h()")
    # TODO: verify expected: true | 84
    assert not r.reverted

def test_library_delegatecall_guard_view_staticcall(harness):
    """libraries/contracts/library_delegatecall_guard_view_staticcall.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_delegatecall_guard_view_staticcall.sol")
    # f() -> 42
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 42
    # g() -> true, 42
    r = harness.call(app, "g()")
    # TODO: verify expected: true | 42
    assert not r.reverted
    # h() -> true, 42
    r = harness.call(app, "h()")
    # TODO: verify expected: true | 42
    assert not r.reverted

def test_library_enum_as_an_expression(harness):
    """libraries/contracts/library_enum_as_an_expression.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_enum_as_an_expression.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_library_function_selectors(harness):
    """libraries/contracts/library_function_selectors.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_function_selectors.sol")
    # f() -> false, true, 0
    r = harness.call(app, "f()")
    # TODO: verify expected: false | true | 0
    assert not r.reverted
    # g() -> false, true, 0
    r = harness.call(app, "g()")
    # TODO: verify expected: false | true | 0
    assert not r.reverted
    # h() -> false, true, 0
    r = harness.call(app, "h()")
    # TODO: verify expected: false | true | 0
    assert not r.reverted

def test_library_function_selectors_struct(harness):
    """libraries/contracts/library_function_selectors_struct.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_function_selectors_struct.sol")
    # f() -> false, true, 0
    r = harness.call(app, "f()")
    # TODO: verify expected: false | true | 0
    assert not r.reverted
    # g() -> false, true, 0
    r = harness.call(app, "g()")
    # TODO: verify expected: false | true | 0
    assert not r.reverted

def test_library_references_preserve(harness):
    """libraries/contracts/library_references_preserve.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_references_preserve.sol")
    # aSum() -> 4
    r = harness.call(app, "aSum()")
    assert as_int(r.abi_return) == 4
    # bSum() -> 5
    r = harness.call(app, "bSum()")
    assert as_int(r.abi_return) == 5

def test_library_return_struct_with_mapping(harness):
    """libraries/contracts/library_return_struct_with_mapping.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_return_struct_with_mapping.sol")
    # f() -> 123
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 123

def test_library_staticcall_delegatecall(harness):
    """libraries/contracts/library_staticcall_delegatecall.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_staticcall_delegatecall.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_library_stray_values(harness):
    """libraries/contracts/library_stray_values.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_stray_values.sol")
    # f(uint256): 33 -> 0x2a
    r = harness.call(app, "f(uint256)", 33)
    assert as_int(r.abi_return) == 42

def test_library_struct_as_an_expression(harness):
    """libraries/contracts/library_struct_as_an_expression.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_struct_as_an_expression.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_mapping_arguments_in_library(harness):
    """libraries/contracts/mapping_arguments_in_library.sol"""
    app = harness.compile_and_deploy("libraries/contracts/mapping_arguments_in_library.sol")
    # set(uint256,uint256): 1, 42 -> 0
    r = harness.call(app, "set(uint256,uint256)", 1, 42)
    assert as_int(r.abi_return) == 0
    # set(uint256,uint256): 2, 84 -> 0
    r = harness.call(app, "set(uint256,uint256)", 2, 84)
    assert as_int(r.abi_return) == 0
    # set(uint256,uint256): 21, 7 -> 0
    r = harness.call(app, "set(uint256,uint256)", 21, 7)
    assert as_int(r.abi_return) == 0
    # get(uint256): 0 -> 0
    r = harness.call(app, "get(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint256): 1 -> 0x2a
    r = harness.call(app, "get(uint256)", 1)
    assert as_int(r.abi_return) == 42
    # get(uint256): 2 -> 0x54
    r = harness.call(app, "get(uint256)", 2)
    assert as_int(r.abi_return) == 84
    # get(uint256): 21 -> 7
    r = harness.call(app, "get(uint256)", 21)
    assert as_int(r.abi_return) == 7
    # set(uint256,uint256): 1, 21 -> 0x2a
    r = harness.call(app, "set(uint256,uint256)", 1, 21)
    assert as_int(r.abi_return) == 42
    # set(uint256,uint256): 2, 42 -> 0x54
    r = harness.call(app, "set(uint256,uint256)", 2, 42)
    assert as_int(r.abi_return) == 84
    # set(uint256,uint256): 21, 14 -> 7
    r = harness.call(app, "set(uint256,uint256)", 21, 14)
    assert as_int(r.abi_return) == 7
    # get(uint256): 0 -> 0
    r = harness.call(app, "get(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint256): 1 -> 0x15
    r = harness.call(app, "get(uint256)", 1)
    assert as_int(r.abi_return) == 21
    # get(uint256): 2 -> 0x2a
    r = harness.call(app, "get(uint256)", 2)
    assert as_int(r.abi_return) == 42
    # get(uint256): 21 -> 14
    r = harness.call(app, "get(uint256)", 21)
    assert as_int(r.abi_return) == 14

def test_mapping_returns_in_library(harness):
    """libraries/contracts/mapping_returns_in_library.sol"""
    app = harness.compile_and_deploy("libraries/contracts/mapping_returns_in_library.sol")
    # set(bool,uint256,uint256): true, 1, 42 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", True, 1, 42)
    assert as_int(r.abi_return) == 0
    # set(bool,uint256,uint256): true, 2, 84 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", True, 2, 84)
    assert as_int(r.abi_return) == 0
    # set(bool,uint256,uint256): true, 21, 7 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", True, 21, 7)
    assert as_int(r.abi_return) == 0
    # set(bool,uint256,uint256): false, 1, 10 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", False, 1, 10)
    assert as_int(r.abi_return) == 0
    # set(bool,uint256,uint256): false, 2, 11 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", False, 2, 11)
    assert as_int(r.abi_return) == 0
    # set(bool,uint256,uint256): false, 21, 12 -> 0
    r = harness.call(app, "set(bool,uint256,uint256)", False, 21, 12)
    assert as_int(r.abi_return) == 0
    # get(bool,uint256): true, 0 -> 0
    r = harness.call(app, "get(bool,uint256)", True, 0)
    assert as_int(r.abi_return) == 0
    # get(bool,uint256): true, 1 -> 0x2a
    r = harness.call(app, "get(bool,uint256)", True, 1)
    assert as_int(r.abi_return) == 42
    # get(bool,uint256): true, 2 -> 0x54
    r = harness.call(app, "get(bool,uint256)", True, 2)
    assert as_int(r.abi_return) == 84
    # get(bool,uint256): true, 21 -> 7
    r = harness.call(app, "get(bool,uint256)", True, 21)
    assert as_int(r.abi_return) == 7
    # get_a(uint256): 0 -> 0
    r = harness.call(app, "get_a(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get_a(uint256): 1 -> 0x2a
    r = harness.call(app, "get_a(uint256)", 1)
    assert as_int(r.abi_return) == 42
    # get_a(uint256): 2 -> 0x54
    r = harness.call(app, "get_a(uint256)", 2)
    assert as_int(r.abi_return) == 84
    # get_a(uint256): 21 -> 7
    r = harness.call(app, "get_a(uint256)", 21)
    assert as_int(r.abi_return) == 7
    # get(bool,uint256): false, 0 -> 0
    r = harness.call(app, "get(bool,uint256)", False, 0)
    assert as_int(r.abi_return) == 0
    # get(bool,uint256): false, 1 -> 10
    r = harness.call(app, "get(bool,uint256)", False, 1)
    assert as_int(r.abi_return) == 10
    # get(bool,uint256): false, 2 -> 11
    r = harness.call(app, "get(bool,uint256)", False, 2)
    assert as_int(r.abi_return) == 11
    # get(bool,uint256): false, 21 -> 12
    r = harness.call(app, "get(bool,uint256)", False, 21)
    assert as_int(r.abi_return) == 12
    # get_b(uint256): 0 -> 0
    r = harness.call(app, "get_b(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get_b(uint256): 1 -> 10
    r = harness.call(app, "get_b(uint256)", 1)
    assert as_int(r.abi_return) == 10
    # get_b(uint256): 2 -> 11
    r = harness.call(app, "get_b(uint256)", 2)
    assert as_int(r.abi_return) == 11
    # get_b(uint256): 21 -> 12
    r = harness.call(app, "get_b(uint256)", 21)
    assert as_int(r.abi_return) == 12
    # set(bool,uint256,uint256): true, 1, 21 -> 0x2a
    r = harness.call(app, "set(bool,uint256,uint256)", True, 1, 21)
    assert as_int(r.abi_return) == 42
    # set(bool,uint256,uint256): true, 2, 42 -> 0x54
    r = harness.call(app, "set(bool,uint256,uint256)", True, 2, 42)
    assert as_int(r.abi_return) == 84
    # set(bool,uint256,uint256): true, 21, 14 -> 7
    r = harness.call(app, "set(bool,uint256,uint256)", True, 21, 14)
    assert as_int(r.abi_return) == 7
    # set(bool,uint256,uint256): false, 1, 30 -> 10
    r = harness.call(app, "set(bool,uint256,uint256)", False, 1, 30)
    assert as_int(r.abi_return) == 10
    # set(bool,uint256,uint256): false, 2, 31 -> 11
    r = harness.call(app, "set(bool,uint256,uint256)", False, 2, 31)
    assert as_int(r.abi_return) == 11
    # set(bool,uint256,uint256): false, 21, 32 -> 12
    r = harness.call(app, "set(bool,uint256,uint256)", False, 21, 32)
    assert as_int(r.abi_return) == 12
    # get_a(uint256): 0 -> 0
    r = harness.call(app, "get_a(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get_a(uint256): 1 -> 0x15
    r = harness.call(app, "get_a(uint256)", 1)
    assert as_int(r.abi_return) == 21
    # get_a(uint256): 2 -> 0x2a
    r = harness.call(app, "get_a(uint256)", 2)
    assert as_int(r.abi_return) == 42
    # get_a(uint256): 21 -> 14
    r = harness.call(app, "get_a(uint256)", 21)
    assert as_int(r.abi_return) == 14
    # get(bool,uint256): true, 0 -> 0
    r = harness.call(app, "get(bool,uint256)", True, 0)
    assert as_int(r.abi_return) == 0
    # get(bool,uint256): true, 1 -> 0x15
    r = harness.call(app, "get(bool,uint256)", True, 1)
    assert as_int(r.abi_return) == 21
    # get(bool,uint256): true, 2 -> 0x2a
    r = harness.call(app, "get(bool,uint256)", True, 2)
    assert as_int(r.abi_return) == 42
    # get(bool,uint256): true, 21 -> 14
    r = harness.call(app, "get(bool,uint256)", True, 21)
    assert as_int(r.abi_return) == 14
    # get_b(uint256): 0 -> 0
    r = harness.call(app, "get_b(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # get_b(uint256): 1 -> 0x1e
    r = harness.call(app, "get_b(uint256)", 1)
    assert as_int(r.abi_return) == 30
    # get_b(uint256): 2 -> 0x1f
    r = harness.call(app, "get_b(uint256)", 2)
    assert as_int(r.abi_return) == 31
    # get_b(uint256): 21 -> 0x20
    r = harness.call(app, "get_b(uint256)", 21)
    assert as_int(r.abi_return) == 32
    # get(bool,uint256): false, 0 -> 0
    r = harness.call(app, "get(bool,uint256)", False, 0)
    assert as_int(r.abi_return) == 0
    # get(bool,uint256): false, 1 -> 0x1e
    r = harness.call(app, "get(bool,uint256)", False, 1)
    assert as_int(r.abi_return) == 30
    # get(bool,uint256): false, 2 -> 0x1f
    r = harness.call(app, "get(bool,uint256)", False, 2)
    assert as_int(r.abi_return) == 31
    # get(bool,uint256): false, 21 -> 0x20
    r = harness.call(app, "get(bool,uint256)", False, 21)
    assert as_int(r.abi_return) == 32

def test_mapping_returns_in_library_named(harness):
    """libraries/contracts/mapping_returns_in_library_named.sol"""
    app = harness.compile_and_deploy("libraries/contracts/mapping_returns_in_library_named.sol")
    # f() -> 0, 0x2a, 0, 0, 0x15, 0x54
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 0, 42, 0, 0, 21, 84
    assert not r.reverted
    # g() -> 0, 0x2a, 0, 0, 0x15, 0x11
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 0, 42, 0, 0, 21, 17
    assert not r.reverted

def test_payable_function_calls_library(harness):
    """libraries/contracts/payable_function_calls_library.sol"""
    app = harness.compile_and_deploy("libraries/contracts/payable_function_calls_library.sol")
    # f(): 27 -> 7
    r = harness.call(app, "f()", 27)
    assert as_int(r.abi_return) == 7

def test_stub(harness):
    """libraries/contracts/stub.sol"""
    app = harness.compile_and_deploy("libraries/contracts/stub.sol")
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 4
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16

def test_stub_internal(harness):
    """libraries/contracts/stub_internal.sol"""
    app = harness.compile_and_deploy("libraries/contracts/stub_internal.sol")
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 4
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 4
    # g(uint256): 4 -> 16
    r = harness.call(app, "g(uint256)", 4)
    assert as_int(r.abi_return) == 16

def test_using_for_by_name(harness):
    """libraries/contracts/using_for_by_name.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_for_by_name.sol")
    # f(uint256): 7 -> 0x2a
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 42
    # x() -> 0x2a
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 42

def test_using_for_function_on_int(harness):
    """libraries/contracts/using_for_function_on_int.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_for_function_on_int.sol")
    # f(uint256): 9 -> 18
    r = harness.call(app, "f(uint256)", 9)
    assert as_int(r.abi_return) == 18

def test_using_for_overload(harness):
    """libraries/contracts/using_for_overload.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_for_overload.sol")
    # f(uint256): 7 -> 0x2a
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 42
    # x() -> 0x2a
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 42

def test_using_for_storage_structs(harness):
    """libraries/contracts/using_for_storage_structs.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_for_storage_structs.sol")
    # g() -> 7, 7
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 7)

def test_using_library_mappings_public(harness):
    """libraries/contracts/using_library_mappings_public.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_library_mappings_public.sol")
    # f() -> 1, 0, 0x2a, 0x17, 0, 0x63
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 1, 0, 42, 23, 0, 99
    assert not r.reverted

def test_using_library_mappings_return(harness):
    """libraries/contracts/using_library_mappings_return.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_library_mappings_return.sol")
    # f() -> 1, 0, 0x2a, 0x17, 0, 0x63
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 1, 0, 42, 23, 0, 99
    assert not r.reverted

def test_using_library_structs(harness):
    """libraries/contracts/using_library_structs.sol"""
    app = harness.compile_and_deploy("libraries/contracts/using_library_structs.sol")
    # f() -> 7, 8
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)
