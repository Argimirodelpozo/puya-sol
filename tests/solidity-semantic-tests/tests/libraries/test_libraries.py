"""Tests for the libraries category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_attached_internal_library_function_accepting_calldata(harness):
    """libraries/contracts/attached_internal_library_function_accepting_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_internal_library_function_accepting_calldata.sol")
    # Returns two bytes1 values = first char of "abcd" = b"a".
    r = harness.call(app, "f(bytes)", b"abcd")
    assert [bytes(x) for x in r.abi_return] == [b"a", b"a"]

def test_attached_internal_library_function_returning_calldata(harness):
    """libraries/contracts/attached_internal_library_function_returning_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_internal_library_function_returning_calldata.sol")
    r = harness.call(app, "f(bytes)", b"abcd")
    assert [bytes(x) for x in r.abi_return] == [b"a", b"a"]

def test_attached_public_library_function_accepting_calldata_sol(harness):
    """libraries/contracts/attached_public_library_function_accepting_calldata.sol.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_public_library_function_accepting_calldata.sol.sol")
    r = harness.call(app, "f(bytes)", b"abcd")
    assert [bytes(x) for x in r.abi_return] == [b"a", b"a"]

def test_attached_public_library_function_returning_calldata(harness):
    """libraries/contracts/attached_public_library_function_returning_calldata.sol"""
    app = harness.compile_and_deploy("libraries/contracts/attached_public_library_function_returning_calldata.sol")
    r = harness.call(app, "f(bytes)", b"abcd")
    assert [bytes(x) for x in r.abi_return] == [b"a", b"a"]

def test_external_call_with_function_pointer_parameter(harness):
    """libraries/contracts/external_call_with_function_pointer_parameter.sol"""
    app = harness.compile_and_deploy("libraries/contracts/external_call_with_function_pointer_parameter.sol")
    r = harness.call(app, "g(uint256)", 4, extra_fee=5000)
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
    addr_a = encoding.encode_address((97434929227759267208256849212272652248082393770).to_bytes(32, "big"))
    zero = encoding.encode_address(b"\x00" * 32)
    # foo(a, a) -> true (same address)
    r = harness.call(app, "foo(address,address)", addr_a, addr_a)
    assert bool(as_int(r.abi_return)) is True
    # foo(a, 0) -> false (different addresses)
    r = harness.call(app, "foo(address,address)", addr_a, zero)
    assert bool(as_int(r.abi_return)) is False

def test_internal_library_function_attached_to_address_named_send_transfer(harness):
    """libraries/contracts/internal_library_function_attached_to_address_named_send_transfer.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_address_named_send_transfer.sol")
    # useTransfer(address): 0x111122223333444455556666777788889999aAaa ->
    r = harness.call(app, "useTransfer(address)", encoding.encode_address((97434929227759267208256849212272652248082393770).to_bytes(32, "big")))
    # (void return — call succeeding is the assertion)
    # useSend(address): 0x111122223333444455556666777788889999aAaa ->
    r = harness.call(app, "useSend(address)", encoding.encode_address((97434929227759267208256849212272652248082393770).to_bytes(32, "big")))
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
    app = harness.compile_and_deploy(
        "libraries/contracts/internal_library_function_attached_to_external_function_type.sol",
        postinit_inner_txns=4,
    )
    r = harness.call(app, "test(uint256)", 5, extra_fee=5000)
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
    # sum(bytes2, bytes2) returns bytes2 = OR of the two args.
    r = harness.call(app, "sum(bytes2,bytes2)", b"\x11\x00", b"\x00\x22")
    assert bytes(r.abi_return) == b"\x11\x22"

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
    r = harness.call(app, "test(uint256)", 5)
    assert as_int(r.abi_return) == 10

def test_internal_library_function_attached_to_internal_function_type_named_selector(harness):
    """libraries/contracts/internal_library_function_attached_to_internal_function_type_named_selector.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_internal_function_type_named_selector.sol")
    r = harness.call(app, "test(uint256)", 5)
    assert as_int(r.abi_return) == 10

def test_internal_library_function_attached_to_literal(harness):
    """libraries/contracts/internal_library_function_attached_to_literal.sol"""
    app = harness.compile_and_deploy("libraries/contracts/internal_library_function_attached_to_literal.sol")
    assert as_int(harness.call(app, "double42()").abi_return) == 84
    # doubleABC() returns bytes "abcabc" — algosdk decodes as list[int].
    assert bytes(harness.call(app, "doubleABC()").abi_return) == b"abcabc"

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
    app = harness.compile_and_deploy('libraries/contracts/library_address.sol')

def test_library_address_homestead(harness):
    """libraries/contracts/library_address_homestead.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_address_homestead.sol')

def test_library_address_via_module(harness):
    """libraries/contracts/library_address_via_module.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_address_via_module.sol')

def test_library_call_in_homestead(harness):
    """libraries/contracts/library_call_in_homestead.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_call_in_homestead.sol")
    # f() stores msg.sender into state — the library call delegates, so
    # the sender is the test runner's account, not a fixed EVM address.
    harness.call(app, "f()")
    r = harness.call(app, "sender()")
    assert r.abi_return == harness.localnet.account.address

def test_library_delegatecall_guard_pure(harness):
    """libraries/contracts/library_delegatecall_guard_pure.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_delegatecall_guard_pure.sol')

def test_library_delegatecall_guard_view_needed(harness):
    """libraries/contracts/library_delegatecall_guard_view_needed.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_delegatecall_guard_view_needed.sol')

def test_library_delegatecall_guard_view_not_needed(harness):
    """libraries/contracts/library_delegatecall_guard_view_not_needed.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_delegatecall_guard_view_not_needed.sol')

def test_library_delegatecall_guard_view_staticcall(harness):
    """libraries/contracts/library_delegatecall_guard_view_staticcall.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_delegatecall_guard_view_staticcall.sol')

def test_library_enum_as_an_expression(harness):
    """libraries/contracts/library_enum_as_an_expression.sol"""
    app = harness.compile_and_deploy("libraries/contracts/library_enum_as_an_expression.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_library_function_selectors(harness):
    """libraries/contracts/library_function_selectors.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_function_selectors.sol')

def test_library_function_selectors_struct(harness):
    """libraries/contracts/library_function_selectors_struct.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_function_selectors_struct.sol')

def test_library_references_preserve(harness):
    """libraries/contracts/library_references_preserve.sol"""
    # ctor `new A()` + `new B()` performs two child-app deployments, each
    # with their own create-txn + funding payment — needs extra inner-txn
    # fee budget on the __postInit call.
    app = harness.compile_and_deploy(
        "libraries/contracts/library_references_preserve.sol",
        postinit_inner_txns=8,
    )
    # aSum() -> 4
    r = harness.call(app, "aSum()")
    assert as_int(r.abi_return) == 4
    # bSum() -> 5
    r = harness.call(app, "bSum()")
    assert as_int(r.abi_return) == 5

def test_library_return_struct_with_mapping(harness):
    """libraries/contracts/library_return_struct_with_mapping.sol"""
    app = harness.compile_and_deploy('libraries/contracts/library_return_struct_with_mapping.sol')

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
    app = harness.compile_and_deploy('libraries/contracts/using_library_structs.sol')
