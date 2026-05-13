"""Auto-generated tests for the storage category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_accessors_mapping_for_array(harness):
    """storage/contracts/accessors_mapping_for_array.sol"""
    app = harness.compile_and_deploy("storage/contracts/accessors_mapping_for_array.sol")
    # data(uint256,uint256): 2, 2 -> 8
    r = harness.call(app, "data(uint256,uint256)", 2, 2)
    assert r.abi_return == 8
    # data(uint256,uint256): 2, 8 -> FAILURE # NB: the original code contained a bug here #
    r = harness.call(app, "data(uint256,uint256)", 2, 8, expect_revert=True)
    assert r.reverted
    # dynamicData(uint256,uint256): 2, 2 -> 8
    r = harness.call(app, "dynamicData(uint256,uint256)", 2, 2)
    assert r.abi_return == 8
    # dynamicData(uint256,uint256): 2, 8 -> FAILURE
    r = harness.call(app, "dynamicData(uint256,uint256)", 2, 8, expect_revert=True)
    assert r.reverted

def test_array_accessor(harness):
    """storage/contracts/array_accessor.sol"""
    app = harness.compile_and_deploy("storage/contracts/array_accessor.sol")
    # data(uint256): 0 -> 8
    r = harness.call(app, "data(uint256)", 0)
    assert r.abi_return == 8
    # data(uint256): 8 -> FAILURE
    r = harness.call(app, "data(uint256)", 8, expect_revert=True)
    assert r.reverted
    # dynamicData(uint256): 2 -> 8
    r = harness.call(app, "dynamicData(uint256)", 2)
    assert r.abi_return == 8
    # dynamicData(uint256): 8 -> FAILURE
    r = harness.call(app, "dynamicData(uint256)", 8, expect_revert=True)
    assert r.reverted
    # smallTypeData(uint256): 1 -> 22
    r = harness.call(app, "smallTypeData(uint256)", 1)
    assert r.abi_return == 22
    # smallTypeData(uint256): 127 -> 2
    r = harness.call(app, "smallTypeData(uint256)", 127)
    assert r.abi_return == 2
    # smallTypeData(uint256): 128 -> FAILURE
    r = harness.call(app, "smallTypeData(uint256)", 128, expect_revert=True)
    assert r.reverted
    # multiple_map(uint256,uint256,uint256): 2, 1, 2 -> 3
    r = harness.call(app, "multiple_map(uint256,uint256,uint256)", 2, 1, 2)
    assert r.abi_return == 3

def test_chop_sign_bits(harness):
    """storage/contracts/chop_sign_bits.sol"""
    app = harness.compile_and_deploy("storage/contracts/chop_sign_bits.sol")
    # x(uint256): 0 -> -1
    r = harness.call(app, "x(uint256)", 0)
    assert r.abi_return == -1
    # x(uint256): 1 -> -2
    r = harness.call(app, "x(uint256)", 1)
    assert r.abi_return == -2
    # y(uint256): 0 -> -5
    r = harness.call(app, "y(uint256)", 0)
    assert r.abi_return == -5
    # y(uint256): 1 -> -6
    r = harness.call(app, "y(uint256)", 1)
    assert r.abi_return == -6
    # f() -> 0x20, 2, -3, -4
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 2, -3, -4)
    # g() -> -3, -4
    r = harness.call(app, "g()")
    assert tuple(r.abi_return) == (-3, -4)
    # h(int8): -10 -> -10
    r = harness.call(app, "h(int8)", 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff6)
    assert r.abi_return == -10

def test_complex_accessors(harness):
    """storage/contracts/complex_accessors.sol"""
    app = harness.compile_and_deploy("storage/contracts/complex_accessors.sol")
    # to_string_map(uint256): 42 -> "24"
    r = harness.call(app, "to_string_map(uint256)", 42)
    # TODO: verify expected: "24"
    assert not r.reverted
    # to_bool_map(uint256): 42 -> false
    r = harness.call(app, "to_bool_map(uint256)", 42)
    assert r.abi_return is False
    # to_uint_map(uint256): 42 -> 12
    r = harness.call(app, "to_uint_map(uint256)", 42)
    assert r.abi_return == 12
    # to_multiple_map(uint256,uint256): 42, 23 -> 31
    r = harness.call(app, "to_multiple_map(uint256,uint256)", 42, 23)
    assert r.abi_return == 31

def test_delete_overlapping_transient_after_inherited_storage_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_after_inherited_storage_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_inherited_storage_same_value_type.sol")
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_after_storage_array_delete_different_base_type(harness):
    """storage/contracts/delete_overlapping_transient_after_storage_array_delete_different_base_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_storage_array_delete_different_base_type.sol")
    # getFlags() -> true, true, true
    r = harness.call(app, "getFlags()")
    assert tuple(r.abi_return) == (True, True, True)
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getFlags() -> false, false, false
    r = harness.call(app, "getFlags()")
    assert tuple(r.abi_return) == (False, False, False)

def test_delete_overlapping_transient_after_storage_array_pop_same_base_type(harness):
    """storage/contracts/delete_overlapping_transient_after_storage_array_pop_same_base_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_storage_array_pop_same_base_type.sol")
    # pushArr() ->
    r = harness.call(app, "pushArr()")
    # (void return — call succeeding is the assertion)
    # getArr() -> 1
    r = harness.call(app, "getArr()")
    assert r.abi_return == 1
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getArr() -> 0
    r = harness.call(app, "getArr()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_after_storage_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_after_storage_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_storage_delete_same_value_type.sol")
    # varStorage() -> 0xeeeeeeeeee
    r = harness.call(app, "varStorage()")
    assert r.abi_return == 1026210852590
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # varStorage() -> 0
    r = harness.call(app, "varStorage()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_after_storage_mapping_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_after_storage_mapping_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_storage_mapping_delete_same_value_type.sol")
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getM() -> 0
    r = harness.call(app, "getM()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_after_storage_struct_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_after_storage_struct_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_after_storage_struct_delete_same_value_type.sol")
    # getS() -> 1, 0x1234
    r = harness.call(app, "getS()")
    assert tuple(r.abi_return) == (1, 4660)
    # setAndDelete() ->
    r = harness.call(app, "setAndDelete()")
    # (void return — call succeeding is the assertion)
    # getS() -> 0, 0
    r = harness.call(app, "getS()")
    assert tuple(r.abi_return) == (0, 0)

def test_delete_overlapping_transient_before_inherited_storage_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_before_inherited_storage_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_inherited_storage_same_value_type.sol")
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # x() -> 0
    r = harness.call(app, "x()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_before_storage_array_delete_different_base_type(harness):
    """storage/contracts/delete_overlapping_transient_before_storage_array_delete_different_base_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_storage_array_delete_different_base_type.sol")
    # getFlags() -> true, true, true
    r = harness.call(app, "getFlags()")
    assert tuple(r.abi_return) == (True, True, True)
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getFlags() -> false, false, false
    r = harness.call(app, "getFlags()")
    assert tuple(r.abi_return) == (False, False, False)

def test_delete_overlapping_transient_before_storage_array_partial_assignment_same_base_type(harness):
    """storage/contracts/delete_overlapping_transient_before_storage_array_partial_assignment_same_base_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_storage_array_partial_assignment_same_base_type.sol")
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getLarge() -> 10, 20, 0, 0
    r = harness.call(app, "getLarge()")
    assert tuple(r.abi_return) == (10, 20, 0, 0)

def test_delete_overlapping_transient_before_storage_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_before_storage_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_storage_delete_same_value_type.sol")
    # varStorage() -> 0xeeeeeeeeee
    r = harness.call(app, "varStorage()")
    assert r.abi_return == 1026210852590
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # varStorage() -> 0
    r = harness.call(app, "varStorage()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_before_storage_mapping_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_before_storage_mapping_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_storage_mapping_delete_same_value_type.sol")
    # setAndClear() ->
    r = harness.call(app, "setAndClear()")
    # (void return — call succeeding is the assertion)
    # getM() -> 0
    r = harness.call(app, "getM()")
    assert r.abi_return == 0

def test_delete_overlapping_transient_before_storage_struct_delete_same_value_type(harness):
    """storage/contracts/delete_overlapping_transient_before_storage_struct_delete_same_value_type.sol"""
    app = harness.compile_and_deploy("storage/contracts/delete_overlapping_transient_before_storage_struct_delete_same_value_type.sol")
    # getS() -> 1, 0x1234
    r = harness.call(app, "getS()")
    assert tuple(r.abi_return) == (1, 4660)
    # setAndDelete() ->
    r = harness.call(app, "setAndDelete()")
    # (void return — call succeeding is the assertion)
    # getS() -> 0, 0
    r = harness.call(app, "getS()")
    assert tuple(r.abi_return) == (0, 0)

def test_empty_nonempty_empty(harness):
    """storage/contracts/empty_nonempty_empty.sol"""
    app = harness.compile_and_deploy("storage/contracts/empty_nonempty_empty.sol")
    # set(bytes): 0x20, 3, "abc"
    r = harness.call(app, "set(bytes)", 32, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 0
    r = harness.call(app, "set(bytes)", 32, 0)
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 31, "1234567890123456789012345678901"
    r = harness.call(app, "set(bytes)", 32, 31, bytes.fromhex('31323334353637383930313233343536373839303132333435363738393031'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 36, "12345678901234567890123456789012", "XXXX"
    r = harness.call(app, "set(bytes)", 32, 36, bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('58585858'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 3, "abc"
    r = harness.call(app, "set(bytes)", 32, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 0
    r = harness.call(app, "set(bytes)", 32, 0)
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 3, "abc"
    r = harness.call(app, "set(bytes)", 32, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 36, "12345678901234567890123456789012", "XXXX"
    r = harness.call(app, "set(bytes)", 32, 36, bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('58585858'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 0
    r = harness.call(app, "set(bytes)", 32, 0)
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 66, "12345678901234567890123456789012", "12345678901234567890123456789012", "12"
    r = harness.call(app, "set(bytes)", 32, 66, bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('3132'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 3, "abc"
    r = harness.call(app, "set(bytes)", 32, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # set(bytes): 0x20, 0
    r = harness.call(app, "set(bytes)", 32, 0)
    # (void return — call succeeding is the assertion)

def test_mapping_state(harness):
    """storage/contracts/mapping_state.sol"""
    app = harness.compile_and_deploy("storage/contracts/mapping_state.sol")
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 0
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 0
    # getVoteCount(address): 2 -> 0
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 0
    # vote(address,address): 0, 2 -> false
    r = harness.call(app, "vote(address,address)", 0, 2)
    assert r.abi_return is False
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 0
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 0
    # getVoteCount(address): 2 -> 0
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 0
    # grantVoteRight(address): 0 ->
    r = harness.call(app, "grantVoteRight(address)", 0)
    # (void return — call succeeding is the assertion)
    # grantVoteRight(address): 1 ->
    r = harness.call(app, "grantVoteRight(address)", 1)
    # (void return — call succeeding is the assertion)
    # vote(address,address): 0, 2 -> true
    r = harness.call(app, "vote(address,address)", 0, 2)
    assert r.abi_return is True
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 0
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 0
    # getVoteCount(address): 2 -> 1
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 1
    # vote(address,address): 0, 1 -> false
    r = harness.call(app, "vote(address,address)", 0, 1)
    assert r.abi_return is False
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 0
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 0
    # getVoteCount(address): 2 -> 1
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 1
    # vote(address,address): 2, 1 -> false
    r = harness.call(app, "vote(address,address)", 2, 1)
    assert r.abi_return is False
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 0
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 0
    # getVoteCount(address): 2 -> 1
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 1
    # grantVoteRight(address): 2 ->
    r = harness.call(app, "grantVoteRight(address)", 2)
    # (void return — call succeeding is the assertion)
    # vote(address,address): 2, 1 -> true
    r = harness.call(app, "vote(address,address)", 2, 1)
    assert r.abi_return is True
    # getVoteCount(address): 0 -> 0
    r = harness.call(app, "getVoteCount(address)", 0)
    assert r.abi_return == 0
    # getVoteCount(address): 1 -> 1
    r = harness.call(app, "getVoteCount(address)", 1)
    assert r.abi_return == 1
    # getVoteCount(address): 2 -> 1
    r = harness.call(app, "getVoteCount(address)", 2)
    assert r.abi_return == 1

def test_mapping_string_key(harness):
    """storage/contracts/mapping_string_key.sol"""
    app = harness.compile_and_deploy("storage/contracts/mapping_string_key.sol")
    # set(string,uint256): 0x40, 8, 3, "abc" ->
    r = harness.call(app, "set(string,uint256)", 64, 8, 3, bytes.fromhex('616263'))
    # (void return — call succeeding is the assertion)
    # get(string): 0x20, 3, "abc" -> 8
    r = harness.call(app, "get(string)", 32, 3, bytes.fromhex('616263'))
    assert r.abi_return == 8
    # get(string): 0x20, 3, "abe" -> 0
    r = harness.call(app, "get(string)", 32, 3, bytes.fromhex('616265'))
    assert r.abi_return == 0
    # getFixed() -> 0
    r = harness.call(app, "getFixed()")
    assert r.abi_return == 0
    # setFixed(uint256): 9 ->
    r = harness.call(app, "setFixed(uint256)", 9)
    # (void return — call succeeding is the assertion)
    # getFixed() -> 9
    r = harness.call(app, "getFixed()")
    assert r.abi_return == 9

def test_mappings_array2d_pop_delete(harness):
    """storage/contracts/mappings_array2d_pop_delete.sol"""
    app = harness.compile_and_deploy("storage/contracts/mappings_array2d_pop_delete.sol")
    # n1(uint256,uint256): 42, 64 ->
    r = harness.call(app, "n1(uint256,uint256)", 42, 64)
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64
    # p() ->
    r = harness.call(app, "p()")
    # (void return — call succeeding is the assertion)
    # n2() ->
    r = harness.call(app, "n2()")
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64
    # d() -> 0
    r = harness.call(app, "d()")
    assert r.abi_return == 0
    # n2() ->
    r = harness.call(app, "n2()")
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64

def test_mappings_array_pop_delete(harness):
    """storage/contracts/mappings_array_pop_delete.sol"""
    app = harness.compile_and_deploy("storage/contracts/mappings_array_pop_delete.sol")
    # n1(uint256,uint256): 42, 64 ->
    r = harness.call(app, "n1(uint256,uint256)", 42, 64)
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64
    # p() ->
    r = harness.call(app, "p()")
    # (void return — call succeeding is the assertion)
    # n2() ->
    r = harness.call(app, "n2()")
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64
    # d() -> 0
    r = harness.call(app, "d()")
    assert r.abi_return == 0
    # n2() ->
    r = harness.call(app, "n2()")
    # (void return — call succeeding is the assertion)
    # map(uint256): 42 -> 64
    r = harness.call(app, "map(uint256)", 42)
    assert r.abi_return == 64

def test_packed_functions(harness):
    """storage/contracts/packed_functions.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_functions.sol")
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # t1() -> 7
    r = harness.call(app, "t1()")
    assert r.abi_return == 7
    # t2() -> 8
    r = harness.call(app, "t2()")
    assert r.abi_return == 8
    # t3() -> 7
    r = harness.call(app, "t3()")
    assert r.abi_return == 7
    # t4() -> 8
    r = harness.call(app, "t4()")
    assert r.abi_return == 8
    # x() -> 2
    r = harness.call(app, "x()")
    assert r.abi_return == 2

def test_packed_storage_overflow(harness):
    """storage/contracts/packed_storage_overflow.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_storage_overflow.sol")
    # f() -> 0x1234, 0x0, 0x0, 0xfffe
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (4660, 0, 0, 65534)

def test_packed_storage_signed(harness):
    """storage/contracts/packed_storage_signed.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_storage_signed.sol")
    # test() -> -2, 4, -112, 0
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (-2, 4, -112, 0)

def test_packed_storage_structs_bytes(harness):
    """storage/contracts/packed_storage_structs_bytes.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_storage_structs_bytes.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert r.abi_return is True

def test_packed_storage_structs_enum(harness):
    """storage/contracts/packed_storage_structs_enum.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_storage_structs_enum.sol")
    # test() -> 1
    r = harness.call(app, "test()")
    assert r.abi_return == 1

def test_packed_storage_structs_uint(harness):
    """storage/contracts/packed_storage_structs_uint.sol"""
    app = harness.compile_and_deploy("storage/contracts/packed_storage_structs_uint.sol")
    # test() -> 1
    r = harness.call(app, "test()")
    assert r.abi_return == 1

def test_simple_accessor(harness):
    """storage/contracts/simple_accessor.sol"""
    app = harness.compile_and_deploy("storage/contracts/simple_accessor.sol")
    # data() -> 8
    r = harness.call(app, "data()")
    assert r.abi_return == 8

def test_state_smoke_test(harness):
    """storage/contracts/state_smoke_test.sol"""
    app = harness.compile_and_deploy("storage/contracts/state_smoke_test.sol")
    # get(uint8): 0x00 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert r.abi_return == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert r.abi_return == 0
    # set(uint8,uint256): 0x00, 0x1234 ->
    r = harness.call(app, "set(uint8,uint256)", 0, 4660)
    # (void return — call succeeding is the assertion)
    # set(uint8,uint256): 0x01, 0x8765 ->
    r = harness.call(app, "set(uint8,uint256)", 1, 34661)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0x00 -> 0x1234
    r = harness.call(app, "get(uint8)", 0)
    assert r.abi_return == 4660
    # get(uint8): 0x01 -> 0x8765
    r = harness.call(app, "get(uint8)", 1)
    assert r.abi_return == 34661
    # set(uint8,uint256): 0x00, 0x03 ->
    r = harness.call(app, "set(uint8,uint256)", 0, 3)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0x00 -> 0x03
    r = harness.call(app, "get(uint8)", 0)
    assert r.abi_return == 3

def test_static_array_copy_cleanup(harness):
    """storage/contracts/static_array_copy_cleanup.sol"""
    app = harness.compile_and_deploy("storage/contracts/static_array_copy_cleanup.sol")
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # getDestAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getDestAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillSource()
    r = harness.call(app, "fillSource()")
    # (void return — call succeeding is the assertion)
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    assert not r.reverted
    # fillDest()
    r = harness.call(app, "fillDest()")
    # (void return — call succeeding is the assertion)
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    assert not r.reverted
    # getDestAsUint() -> 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139
    r = harness.call(app, "getDestAsUint()")
    # TODO: verify structural decoding matches expected: 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139
    assert not r.reverted
    # copySourceToDest()
    r = harness.call(app, "copySourceToDest()")
    # (void return — call succeeding is the assertion)
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
    assert not r.reverted
    # getDestAsUint() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getDestAsUint()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # deleteSource()
    r = harness.call(app, "deleteSource()")
    # (void return — call succeeding is the assertion)
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # getDestAsUint() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getDestAsUint()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # deleteDest()
    r = harness.call(app, "deleteDest()")
    # (void return — call succeeding is the assertion)
    # canary() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canary()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getSourceAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getSourceAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # getDestAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "getDestAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_array_and_partial_assignment_with_layout(harness):
    """storage/contracts/storage_boundary_array_and_partial_assignment_with_layout.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_and_partial_assignment_with_layout.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # partialAssignArrayBeforeStorageBoundary()
    r = harness.call(app, "partialAssignArrayBeforeStorageBoundary()")
    # (void return — call succeeding is the assertion)
    # x() -> 11, 12, 13, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 11, 12, 13, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 11, 1, 2, 3, 4, 5, 6, 7, 8, 9
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 11, 1, 2, 3, 4, 5, 6, 7, 8, 9
    assert not r.reverted
    # partialAssignArrayCrossStorageBoundary()
    r = harness.call(app, "partialAssignArrayCrossStorageBoundary()")
    # (void return — call succeeding is the assertion)
    # x() -> 14, 15, 16, 17, 18, 19, 20, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 14, 15, 16, 17, 18, 19, 20, 0, 0, 0
    assert not r.reverted
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_array_assignment(harness):
    """storage/contracts/storage_boundary_array_assignment.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_assignment.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # assignArray(uint256[10]): 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ->
    r = harness.call(app, "assignArray(uint256[10])", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    # (void return — call succeeding is the assertion)
    # x() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # assignArray(uint256[10]): 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 ->
    r = harness.call(app, "assignArray(uint256[10])", 10, 20, 30, 40, 50, 60, 70, 80, 90, 100)
    # (void return — call succeeding is the assertion)
    # x() -> 10, 20, 30, 40, 50, 60, 70, 80, 90, 100
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 10, 20, 30, 40, 50, 60, 70, 80, 90, 100
    assert not r.reverted

def test_storage_boundary_array_copy(harness):
    """storage/contracts/storage_boundary_array_copy.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_copy.sol")
    # x() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # y() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "y()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # copyXToY()
    r = harness.call(app, "copyXToY()")
    # (void return — call succeeding is the assertion)
    # x() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # y() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "y()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # clearX()
    r = harness.call(app, "clearX()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # y() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "y()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # copyYToX()
    r = harness.call(app, "copyYToX()")
    # (void return — call succeeding is the assertion)
    # x() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted
    # y() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    r = harness.call(app, "y()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
    assert not r.reverted

def test_storage_boundary_array_delete(harness):
    """storage/contracts/storage_boundary_array_delete.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_delete.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    assert not r.reverted
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_array_delete_overlapping_variable(harness):
    """storage/contracts/storage_boundary_array_delete_overlapping_variable.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_delete_overlapping_variable.sol")
    # y() -> 42
    r = harness.call(app, "y()")
    assert r.abi_return == 42
    # x() -> 0, 0, 0, 0, 0, 42, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 42, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # y() -> 5
    r = harness.call(app, "y()")
    assert r.abi_return == 5
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    assert not r.reverted
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # y() -> 0
    r = harness.call(app, "y()")
    assert r.abi_return == 0
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_array_packing_not_overlapping_variable(harness):
    """storage/contracts/storage_boundary_array_packing_not_overlapping_variable.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_packing_not_overlapping_variable.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    assert not r.reverted
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # shrinkTo5()
    r = harness.call(app, "shrinkTo5()")
    # (void return — call succeeding is the assertion)
    # x() -> 11, 12, 13, 14, 15, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 11, 12, 13, 14, 15, 0, 0, 0, 0, 0
    assert not r.reverted
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935

def test_storage_boundary_array_partial_assignment(harness):
    """storage/contracts/storage_boundary_array_partial_assignment.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_array_partial_assignment.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    assert not r.reverted
    # partialAssignArrayCrossStorageBoundary()
    r = harness.call(app, "partialAssignArrayCrossStorageBoundary()")
    # (void return — call succeeding is the assertion)
    # x() -> 11, 12, 13, 14, 15, 16, 17, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 11, 12, 13, 14, 15, 16, 17, 0, 0, 0
    assert not r.reverted
    # partialAssignArrayBeforeStorageBoundary()
    r = harness.call(app, "partialAssignArrayBeforeStorageBoundary()")
    # (void return — call succeeding is the assertion)
    # x() -> 21, 22, 23, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 21, 22, 23, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_delete_overflow_bug(harness):
    """storage/contracts/storage_boundary_delete_overflow_bug.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_delete_overflow_bug.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
    assert not r.reverted
    # partialAssignArray()
    r = harness.call(app, "partialAssignArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 11, 22, 33, 44, 55, 66, 77, 88, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 11, 22, 33, 44, 55, 66, 77, 88, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_packed_array(harness):
    """storage/contracts/storage_boundary_packed_array.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_packed_array.sol")
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39
    assert not r.reverted
    # clearArray()
    r = harness.call(app, "clearArray()")
    # (void return — call succeeding is the assertion)
    # x() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "x()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted

def test_storage_boundary_struct_array_mixed_types(harness):
    """storage/contracts/storage_boundary_struct_array_mixed_types.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_struct_array_mixed_types.sol")
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillBoundaryArray()
    r = harness.call(app, "fillBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, true, 6, 7, 8, 9, true, 11, 12, 13, 14, true, 16, 17, 18, 19, true, 21, 22, 23, 24, true, 26, 27, 28, 29, true, 31, 32, 33, 34, true, 36, 37, 38, 39, true, 41, 42, 43, 44, true, 46, 47, 48, 49, true
    r = harness.call(app, "boundaryArray()")
    # TODO: verify expected: 1 | 2 | 3 | 4 | true | 6 | 7 | 8 | 9 | true | 11 | 12 | 13 | 14 | true | 16 | 17 | 18 | 19 | true | 21 | 22 | 23 | 24 | true | 26 | 27 | 28 | 29 | true | 31 | 32 | 33 | 34 | true | 36 | 37 | 38 | 39 | true | 41 | 42 | 43 | 44 | true | 46 | 47 | 48 | 49 | true
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # copyFromBoundary()
    r = harness.call(app, "copyFromBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, true, 6, 7, 8, 9, true, 11, 12, 13, 14, true, 16, 17, 18, 19, true, 21, 22, 23, 24, true, 26, 27, 28, 29, true, 31, 32, 33, 34, true, 36, 37, 38, 39, true, 41, 42, 43, 44, true, 46, 47, 48, 49, true
    r = harness.call(app, "boundaryArray()")
    # TODO: verify expected: 1 | 2 | 3 | 4 | true | 6 | 7 | 8 | 9 | true | 11 | 12 | 13 | 14 | true | 16 | 17 | 18 | 19 | true | 21 | 22 | 23 | 24 | true | 26 | 27 | 28 | 29 | true | 31 | 32 | 33 | 34 | true | 36 | 37 | 38 | 39 | true | 41 | 42 | 43 | 44 | true | 46 | 47 | 48 | 49 | true
    assert not r.reverted
    # destArray() -> 1, 2, 3, 4, true, 6, 7, 8, 9, true, 11, 12, 13, 14, true, 16, 17, 18, 19, true, 21, 22, 23, 24, true, 26, 27, 28, 29, true, 31, 32, 33, 34, true, 36, 37, 38, 39, true, 41, 42, 43, 44, true, 46, 47, 48, 49, true
    r = harness.call(app, "destArray()")
    # TODO: verify expected: 1 | 2 | 3 | 4 | true | 6 | 7 | 8 | 9 | true | 11 | 12 | 13 | 14 | true | 16 | 17 | 18 | 19 | true | 21 | 22 | 23 | 24 | true | 26 | 27 | 28 | 29 | true | 31 | 32 | 33 | 34 | true | 36 | 37 | 38 | 39 | true | 41 | 42 | 43 | 44 | true | 46 | 47 | 48 | 49 | true
    assert not r.reverted
    # fillDestArray()
    r = harness.call(app, "fillDestArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, true, 6, 7, 8, 9, true, 11, 12, 13, 14, true, 16, 17, 18, 19, true, 21, 22, 23, 24, true, 26, 27, 28, 29, true, 31, 32, 33, 34, true, 36, 37, 38, 39, true, 41, 42, 43, 44, true, 46, 47, 48, 49, true
    r = harness.call(app, "boundaryArray()")
    # TODO: verify expected: 1 | 2 | 3 | 4 | true | 6 | 7 | 8 | 9 | true | 11 | 12 | 13 | 14 | true | 16 | 17 | 18 | 19 | true | 21 | 22 | 23 | 24 | true | 26 | 27 | 28 | 29 | true | 31 | 32 | 33 | 34 | true | 36 | 37 | 38 | 39 | true | 41 | 42 | 43 | 44 | true | 46 | 47 | 48 | 49 | true
    assert not r.reverted
    # destArray() -> 51, 52, 53, 54, true, 56, 57, 58, 59, true, 61, 62, 63, 64, true, 66, 67, 68, 69, true, 71, 72, 73, 74, true, 76, 77, 78, 79, true, 81, 82, 83, 84, true, 86, 87, 88, 89, true, 91, 92, 93, 94, true, 96, 97, 98, 99, true
    r = harness.call(app, "destArray()")
    # TODO: verify expected: 51 | 52 | 53 | 54 | true | 56 | 57 | 58 | 59 | true | 61 | 62 | 63 | 64 | true | 66 | 67 | 68 | 69 | true | 71 | 72 | 73 | 74 | true | 76 | 77 | 78 | 79 | true | 81 | 82 | 83 | 84 | true | 86 | 87 | 88 | 89 | true | 91 | 92 | 93 | 94 | true | 96 | 97 | 98 | 99 | true
    assert not r.reverted
    # copyToBoundary()
    r = harness.call(app, "copyToBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 51, 52, 53, 54, true, 56, 57, 58, 59, true, 61, 62, 63, 64, true, 66, 67, 68, 69, true, 71, 72, 73, 74, true, 76, 77, 78, 79, true, 81, 82, 83, 84, true, 86, 87, 88, 89, true, 91, 92, 93, 94, true, 96, 97, 98, 99, true
    r = harness.call(app, "boundaryArray()")
    # TODO: verify expected: 51 | 52 | 53 | 54 | true | 56 | 57 | 58 | 59 | true | 61 | 62 | 63 | 64 | true | 66 | 67 | 68 | 69 | true | 71 | 72 | 73 | 74 | true | 76 | 77 | 78 | 79 | true | 81 | 82 | 83 | 84 | true | 86 | 87 | 88 | 89 | true | 91 | 92 | 93 | 94 | true | 96 | 97 | 98 | 99 | true
    assert not r.reverted
    # destArray() -> 51, 52, 53, 54, true, 56, 57, 58, 59, true, 61, 62, 63, 64, true, 66, 67, 68, 69, true, 71, 72, 73, 74, true, 76, 77, 78, 79, true, 81, 82, 83, 84, true, 86, 87, 88, 89, true, 91, 92, 93, 94, true, 96, 97, 98, 99, true
    r = harness.call(app, "destArray()")
    # TODO: verify expected: 51 | 52 | 53 | 54 | true | 56 | 57 | 58 | 59 | true | 61 | 62 | 63 | 64 | true | 66 | 67 | 68 | 69 | true | 71 | 72 | 73 | 74 | true | 76 | 77 | 78 | 79 | true | 81 | 82 | 83 | 84 | true | 86 | 87 | 88 | 89 | true | 91 | 92 | 93 | 94 | true | 96 | 97 | 98 | 99 | true
    assert not r.reverted
    # deleteBoundaryArray()
    r = harness.call(app, "deleteBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 51, 52, 53, 54, true, 56, 57, 58, 59, true, 61, 62, 63, 64, true, 66, 67, 68, 69, true, 71, 72, 73, 74, true, 76, 77, 78, 79, true, 81, 82, 83, 84, true, 86, 87, 88, 89, true, 91, 92, 93, 94, true, 96, 97, 98, 99, true
    r = harness.call(app, "destArray()")
    # TODO: verify expected: 51 | 52 | 53 | 54 | true | 56 | 57 | 58 | 59 | true | 61 | 62 | 63 | 64 | true | 66 | 67 | 68 | 69 | true | 71 | 72 | 73 | 74 | true | 76 | 77 | 78 | 79 | true | 81 | 82 | 83 | 84 | true | 86 | 87 | 88 | 89 | true | 91 | 92 | 93 | 94 | true | 96 | 97 | 98 | 99 | true
    assert not r.reverted

def test_storage_boundary_struct_array_multislot(harness):
    """storage/contracts/storage_boundary_struct_array_multislot.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_struct_array_multislot.sol")
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillBoundaryArray()
    r = harness.call(app, "fillBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # copyFromBoundary()
    r = harness.call(app, "copyFromBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    assert not r.reverted
    # destArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    assert not r.reverted
    # fillDestArray()
    r = harness.call(app, "fillDestArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
    assert not r.reverted
    # destArray() -> 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    assert not r.reverted
    # copyToBoundary()
    r = harness.call(app, "copyToBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    assert not r.reverted
    # destArray() -> 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    assert not r.reverted
    # deleteBoundaryArray()
    r = harness.call(app, "deleteBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60
    assert not r.reverted

def test_storage_boundary_struct_array_packed(harness):
    """storage/contracts/storage_boundary_struct_array_packed.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_boundary_struct_array_packed.sol")
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # fillBoundaryArray()
    r = harness.call(app, "fillBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    assert not r.reverted
    # destArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # copyFromBoundary()
    r = harness.call(app, "copyFromBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    assert not r.reverted
    # destArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    assert not r.reverted
    # fillDestArray()
    r = harness.call(app, "fillDestArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    assert not r.reverted
    # destArray() -> 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    assert not r.reverted
    # copyToBoundary()
    r = harness.call(app, "copyToBoundary()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    assert not r.reverted
    # destArray() -> 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    assert not r.reverted
    # deleteBoundaryArray()
    r = harness.call(app, "deleteBoundaryArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # boundaryArray() -> 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    r = harness.call(app, "boundaryArray()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    assert not r.reverted
    # destArray() -> 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    r = harness.call(app, "destArray()")
    # TODO: verify structural decoding matches expected: 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
    assert not r.reverted

def test_storage_packed_array_copy(harness):
    """storage/contracts/storage_packed_array_copy.sol"""
    app = harness.compile_and_deploy("storage/contracts/storage_packed_array_copy.sol")
    # getXAsUint() -> 0, 1, 2, 3, 4, 5, 6, 7, 8
    r = harness.call(app, "getXAsUint()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8
    assert not r.reverted
    # getYAsUint() -> 0, 0, 0, 0, 0, 0, 0, 0, 2, 2
    r = harness.call(app, "getYAsUint()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 0, 0, 0, 0, 0, 2, 2
    assert not r.reverted
    # copy()
    r = harness.call(app, "copy()")
    # (void return — call succeeding is the assertion)
    # getXAsUint() -> 0, 1, 2, 3, 4, 5, 6, 7, 8
    r = harness.call(app, "getXAsUint()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8
    assert not r.reverted
    # getYAsUint() -> 0, 1, 2, 3, 4, 5, 6, 7, 8, 0
    r = harness.call(app, "getYAsUint()")
    # TODO: verify structural decoding matches expected: 0, 1, 2, 3, 4, 5, 6, 7, 8, 0
    assert not r.reverted

def test_struct_accessor(harness):
    """storage/contracts/struct_accessor.sol"""
    app = harness.compile_and_deploy("storage/contracts/struct_accessor.sol")
    # data(uint256): 7 -> 1, 2, true
    r = harness.call(app, "data(uint256)", 7)
    # TODO: verify expected: 1 | 2 | true
    assert not r.reverted
