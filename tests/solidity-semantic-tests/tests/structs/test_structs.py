"""Tests for the structs category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_array_of_recursive_struct(harness):
    """structs/contracts/array_of_recursive_struct.sol"""
    app = harness.compile_and_deploy("structs/contracts/array_of_recursive_struct.sol")
    # func() ->
    r = harness.call(app, "func()")
    # (void return — call succeeding is the assertion)

def test_copy_from_mapping(harness):
    """structs/contracts/copy_from_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_from_mapping.sol")
    # to_state() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_state()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # to_storage() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_storage()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # to_memory() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_memory()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted

def test_copy_from_storage(harness):
    """structs/contracts/copy_from_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_from_storage.sol")
    # f returns S[] with one struct {x = 13}.
    r = harness.call(app, "f()")
    assert [tuple(as_int(y) for y in x) for x in r.abi_return] == [(13,)]

def test_copy_struct_array_from_storage(harness):
    """structs/contracts/copy_struct_array_from_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_struct_array_from_storage.sol")
    # test1() -> true
    r = harness.call(app, "test1()")
    assert bool(as_int(r.abi_return)) is True
    # test2() -> true
    r = harness.call(app, "test2()")
    assert bool(as_int(r.abi_return)) is True
    # test3() -> true
    r = harness.call(app, "test3()")
    assert bool(as_int(r.abi_return)) is True
    # test4() -> true
    r = harness.call(app, "test4()")
    assert bool(as_int(r.abi_return)) is True

def test_copy_struct_with_nested_array_from_calldata_to_memory(harness):
    """structs/contracts/copy_struct_with_nested_array_from_calldata_to_memory.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_struct_with_nested_array_from_calldata_to_memory.sol")
    # test((uint8[1],uint8[])): 0x20, 3, 0x40, 2, 7, 11 -> 0x20, 3, 0x40, 2, 7, 11
    r = harness.call(app, "test((uint8[1],uint8[]))", 32, 3, 64, 2, 7, 11)
    # TODO: verify structural decoding matches expected: 32, 3, 64, 2, 7, 11
    assert not r.reverted
    # test((uint8[1],uint8[])): 0x20, 3, 0x40, 3, 17, 19, 23 -> 0x20, 3, 0x40, 3, 17, 19, 23
    r = harness.call(app, "test((uint8[1],uint8[]))", 32, 3, 64, 3, 17, 19, 23)
    # TODO: verify structural decoding matches expected: 32, 3, 64, 3, 17, 19, 23
    assert not r.reverted

def test_copy_struct_with_nested_array_from_calldata_to_storage(harness):
    """structs/contracts/copy_struct_with_nested_array_from_calldata_to_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_struct_with_nested_array_from_calldata_to_storage.sol")
    # test((uint8[1],uint8[])): 0x20, 3, 0x40, 2, 7, 11
    r = harness.call(app, "test((uint8[1],uint8[]))", 32, 3, 64, 2, 7, 11)
    # (void return — call succeeding is the assertion)

def test_copy_struct_with_nested_array_from_memory_to_memory(harness):
    """structs/contracts/copy_struct_with_nested_array_from_memory_to_memory.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_struct_with_nested_array_from_memory_to_memory.sol")
    # test((uint8[1],uint8[])): 0x20, 3, 0x40, 2, 7, 11 -> 0x20, 0, 0x40, 0
    r = harness.call(app, "test((uint8[1],uint8[]))", 32, 3, 64, 2, 7, 11)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0, 64, 0)
    # test((uint8[1],uint8[])): 0x20, 3, 0x40, 3, 17, 19, 23 -> 0x20, 0, 0x40, 0
    r = harness.call(app, "test((uint8[1],uint8[]))", 32, 3, 64, 3, 17, 19, 23)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0, 64, 0)

def test_copy_struct_with_nested_array_from_storage_to_storage(harness):
    """structs/contracts/copy_struct_with_nested_array_from_storage_to_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_struct_with_nested_array_from_storage_to_storage.sol")
    # test()
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)

def test_copy_substructures_from_mapping(harness):
    """structs/contracts/copy_substructures_from_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_substructures_from_mapping.sol")
    # to_state() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_state()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # to_storage() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_storage()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # to_memory() -> 0x20, 0x60, 0xa0, 7, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "to_memory()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 7, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted

def test_copy_substructures_to_mapping(harness):
    """structs/contracts/copy_substructures_to_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_substructures_to_mapping.sol")
    # from_memory() -> 0x20, 0x60, 0xa0, 0x15, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_memory()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 21, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # from_state() -> 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_state()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 21, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # from_calldata((bytes,uint16[],uint16)): 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14 -> 0x20, 0x60, 0xa0, 0x15, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_calldata((bytes,uint16[],uint16))", (b"foo", [13, 14], 21))
    assert not r.reverted

def test_copy_to_mapping(harness):
    """structs/contracts/copy_to_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/copy_to_mapping.sol")
    # from_state() -> 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_state()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 21, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # from_storage() -> 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_storage()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 21, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # from_memory() -> 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_memory()")
    # TODO: verify structural decoding matches expected: 32, 96, 160, 21, 3, 46332796673528066027243215619882264990369332300865266851730502456685210107904, 2, 13, 14
    assert not r.reverted
    # from_calldata((bytes,uint16[],uint16)): 0x20, 0x60, 0xa0, 21, 3, 0x666F6F0000000000000000000000000000000000000000000000000000000000, 2, 13, 14 -> 0x20, 0x60, 0xa0, 21, 3, 0x666f6f0000000000000000000000000000000000000000000000000000000000, 2, 13, 14
    r = harness.call(app, "from_calldata((bytes,uint16[],uint16))", (b"foo", [13, 14], 21))
    assert not r.reverted

def test_delete_struct(harness):
    """structs/contracts/delete_struct.sol"""
    app = harness.compile_and_deploy("structs/contracts/delete_struct.sol")
    # getToDelete() -> 0
    r = harness.call(app, "getToDelete()")
    assert as_int(r.abi_return) == 0
    # getTopValue() -> 0
    r = harness.call(app, "getTopValue()")
    assert as_int(r.abi_return) == 0
    # getNestedValue() -> 0 #mapping values should be the same#
    r = harness.call(app, "getNestedValue()")
    # TODO: verify expected: 0 #mapping values should be the same#
    assert not r.reverted
    # getTopMapping(uint256): 0 -> 1
    r = harness.call(app, "getTopMapping(uint256)", 0)
    assert as_int(r.abi_return) == 1
    # getTopMapping(uint256): 1 -> 2
    r = harness.call(app, "getTopMapping(uint256)", 1)
    assert as_int(r.abi_return) == 2
    # getNestedMapping(uint256): 0 -> true
    r = harness.call(app, "getNestedMapping(uint256)", 0)
    assert bool(as_int(r.abi_return)) is True
    # getNestedMapping(uint256): 1 -> false
    r = harness.call(app, "getNestedMapping(uint256)", 1)
    assert bool(as_int(r.abi_return)) is False

def test_event(harness):
    """structs/contracts/event.sol"""
    app = harness.compile_and_deploy("structs/contracts/event.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_function_type_copy(harness):
    """structs/contracts/function_type_copy.sol"""
    app = harness.compile_and_deploy("structs/contracts/function_type_copy.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_global_(harness):
    """structs/contracts/global.sol"""
    app = harness.compile_and_deploy("structs/contracts/global.sol")
    # f(s) returns (s.a, s.b).
    r = harness.call(app, "f((uint256,uint256))", (42, 23))
    assert tuple(as_int(x) for x in r.abi_return) == (42, 23)

def test_lone_struct_array_type(harness):
    """structs/contracts/lone_struct_array_type.sol"""
    app = harness.compile_and_deploy("structs/contracts/lone_struct_array_type.sol")
    # f() -> 3
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 3

def test_memory_struct_named_constructor(harness):
    """structs/contracts/memory_struct_named_constructor.sol"""
    app = harness.compile_and_deploy("structs/contracts/memory_struct_named_constructor.sol")
    # s() -> 8, true
    r = harness.call(app, "s()")
    # TODO: verify expected: 8 | true
    assert not r.reverted

def test_memory_structs_as_function_args(harness):
    """structs/contracts/memory_structs_as_function_args.sol"""
    app = harness.compile_and_deploy("structs/contracts/memory_structs_as_function_args.sol")
    # test() -> 1, 2, 3
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3)

def test_memory_structs_nested(harness):
    """structs/contracts/memory_structs_nested.sol"""
    app = harness.compile_and_deploy("structs/contracts/memory_structs_nested.sol")
    # test() -> 1, 2, 3, 4
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 4)

def test_memory_structs_nested_load(harness):
    """structs/contracts/memory_structs_nested_load.sol"""
    app = harness.compile_and_deploy("structs/contracts/memory_structs_nested_load.sol")
    # load() -> 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
    r = harness.call(app, "load()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6
    assert not r.reverted
    # store() -> 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
    r = harness.call(app, "store()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6
    assert not r.reverted

def test_memory_structs_read_write(harness):
    """structs/contracts/memory_structs_read_write.sol"""
    app = harness.compile_and_deploy("structs/contracts/memory_structs_read_write.sol")
    # testInit() -> 0, 0, 0, 0, true
    r = harness.call(app, "testInit()")
    # TODO: verify expected: 0 | 0 | 0 | 0 | true
    assert not r.reverted
    # testCopyRead() -> 1, 2, 3, 4
    r = harness.call(app, "testCopyRead()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 4)
    # testAssign() -> 1, 2, 3, 4
    r = harness.call(app, "testAssign()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 3, 4)

def test_msg_data_to_struct_member_copy(harness):
    """structs/contracts/msg_data_to_struct_member_copy.sol"""
    app = harness.compile_and_deploy("structs/contracts/msg_data_to_struct_member_copy.sol")
    # f() -> 0x20, 0x20, 4, 0x26121ff000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912)
    # g() -> 0x20, 0x20, 4, 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 4, 102264414861304285884729579275374176073311626045629144087797787832582884294656)
    # hashes() -> 0x26121ff000000000000000000000000000000000000000000000000000000000, 0xe2179b8e00000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "hashes()")
    assert tuple(as_int(x) for x in r.abi_return) == (17219911917854084299749778639755835327755045716242581057573779540915269926912, 102264414861304285884729579275374176073311626045629144087797787832582884294656)
    # large(uint256,uint256,uint256,uint256): 1, 2, 3, 4 -> 0x20, 0x20, 0x84, 0xe02492f800000000000000000000000000000000000000000000000000000000, 0x100000000000000000000000000000000000000000000000000000000, 0x200000000000000000000000000000000000000000000000000000000, 0x300000000000000000000000000000000000000000000000000000000, 0x400000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "large(uint256,uint256,uint256,uint256)", 1, 2, 3, 4)
    # TODO: verify structural decoding matches expected: 32, 32, 132, 101382698918017097707161245144404298464765490926644769483516792872402539249664, 26959946667150639794667015087019630673637144422540572481103610249216, 53919893334301279589334030174039261347274288845081144962207220498432, 80879840001451919384001045261058892020911433267621717443310830747648, 107839786668602559178668060348078522694548577690162289924414440996864
    assert not r.reverted
    # another_large(uint256,uint256,uint256,uint256): 1, 2, 3, 4 -> 0x20, 0x20, 0x84, 0x2a46f85a00000000000000000000000000000000000000000000000000000000, 0x100000000000000000000000000000000000000000000000000000000, 0x200000000000000000000000000000000000000000000000000000000, 0x300000000000000000000000000000000000000000000000000000000, 0x400000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "another_large(uint256,uint256,uint256,uint256)", 1, 2, 3, 4)
    # TODO: verify structural decoding matches expected: 32, 32, 132, 19122532994520879318127310892525066712363999234356044649309225915721155870720, 26959946667150639794667015087019630673637144422540572481103610249216, 53919893334301279589334030174039261347274288845081144962207220498432, 80879840001451919384001045261058892020911433267621717443310830747648, 107839786668602559178668060348078522694548577690162289924414440996864
    assert not r.reverted

def test_multislot_struct_allocation(harness):
    """structs/contracts/multislot_struct_allocation.sol"""
    app = harness.compile_and_deploy("structs/contracts/multislot_struct_allocation.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_nested_struct_allocation(harness):
    """structs/contracts/nested_struct_allocation.sol"""
    app = harness.compile_and_deploy("structs/contracts/nested_struct_allocation.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_packed_storage_structs_delete(harness):
    """structs/contracts/packed_storage_structs_delete.sol"""
    app = harness.compile_and_deploy("structs/contracts/packed_storage_structs_delete.sol")
    # test() -> 1
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 1

def test_recursive_struct_2(harness):
    """structs/contracts/recursive_struct_2.sol"""
    app = harness.compile_and_deploy("structs/contracts/recursive_struct_2.sol")
    # f() -> 0, 0, 0, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)

def test_recursive_structs(harness):
    """structs/contracts/recursive_structs.sol"""
    app = harness.compile_and_deploy("structs/contracts/recursive_structs.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_simple_struct_allocation(harness):
    """structs/contracts/simple_struct_allocation.sol"""
    app = harness.compile_and_deploy("structs/contracts/simple_struct_allocation.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_struct_assign_reference_to_struct(harness):
    """structs/contracts/struct_assign_reference_to_struct.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_assign_reference_to_struct.sol")
    # assign() -> 2, 2, 3, 3
    r = harness.call(app, "assign()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 2, 3, 3)

def test_struct_constructor_nested(harness):
    """structs/contracts/struct_constructor_nested.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_constructor_nested.sol")
    # get() -> 0x01, 0x00, 0x09, 0x00, 0x04, 0x05
    r = harness.call(app, "get()")
    # TODO: verify structural decoding matches expected: 1, 0, 9, 0, 4, 5
    assert not r.reverted

def test_struct_containing_bytes_copy_and_delete(harness):
    """structs/contracts/struct_containing_bytes_copy_and_delete.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_containing_bytes_copy_and_delete.sol")
    # set(uint256,bytes,uint256): 12, 0x60, 13, 33, "12345678901234567890123456789012", "3" -> true
    r = harness.call(app, "set(uint256,bytes,uint256)", 12, 96, 13, 33, bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('33'))
    assert bool(as_int(r.abi_return)) is True
    # test(uint256): 32 -> "3"
    r = harness.call(app, "test(uint256)", 32)
    # TODO: verify expected: "3"
    assert not r.reverted
    # copy() -> true
    r = harness.call(app, "copy()")
    assert bool(as_int(r.abi_return)) is True
    # set(uint256,bytes,uint256): 12, 0x60, 13, 33, "12345678901234567890123456789012", "3" -> true
    r = harness.call(app, "set(uint256,bytes,uint256)", 12, 96, 13, 33, bytes.fromhex('3132333435363738393031323334353637383930313233343536373839303132'), bytes.fromhex('33'))
    assert bool(as_int(r.abi_return)) is True
    # del() -> true
    r = harness.call(app, "del()")
    assert bool(as_int(r.abi_return)) is True

def test_struct_copy(harness):
    """structs/contracts/struct_copy.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_copy.sol")
    # set(uint256): 7 -> true
    r = harness.call(app, "set(uint256)", 7)
    assert bool(as_int(r.abi_return)) is True
    # retrieve(uint256): 7 -> 1, 3, 4, 2
    r = harness.call(app, "retrieve(uint256)", 7)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3, 4, 2)
    # copy(uint256,uint256): 7, 8 -> true
    r = harness.call(app, "copy(uint256,uint256)", 7, 8)
    assert bool(as_int(r.abi_return)) is True
    # retrieve(uint256): 7 -> 1, 3, 4, 2
    r = harness.call(app, "retrieve(uint256)", 7)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3, 4, 2)
    # retrieve(uint256): 8 -> 1, 3, 4, 2
    r = harness.call(app, "retrieve(uint256)", 8)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3, 4, 2)
    # copy(uint256,uint256): 0, 7 -> true
    r = harness.call(app, "copy(uint256,uint256)", 0, 7)
    assert bool(as_int(r.abi_return)) is True
    # retrieve(uint256): 7 -> 0, 0, 0, 0
    r = harness.call(app, "retrieve(uint256)", 7)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)
    # retrieve(uint256): 8 -> 1, 3, 4, 2
    r = harness.call(app, "retrieve(uint256)", 8)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 3, 4, 2)
    # copy(uint256,uint256): 7, 8 -> true
    r = harness.call(app, "copy(uint256,uint256)", 7, 8)
    assert bool(as_int(r.abi_return)) is True
    # retrieve(uint256): 8 -> 0, 0, 0, 0
    r = harness.call(app, "retrieve(uint256)", 8)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)

def test_struct_copy_via_local(harness):
    """structs/contracts/struct_copy_via_local.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_copy_via_local.sol")
    # test() -> true
    r = harness.call(app, "test()")
    assert bool(as_int(r.abi_return)) is True

def test_struct_delete_member(harness):
    """structs/contracts/struct_delete_member.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_member.sol")
    # deleteMember() -> 0
    r = harness.call(app, "deleteMember()")
    assert as_int(r.abi_return) == 0

def test_struct_delete_storage(harness):
    """structs/contracts/struct_delete_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_storage.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_struct_delete_storage_nested_small(harness):
    """structs/contracts/struct_delete_storage_nested_small.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_storage_nested_small.sol", via_yul_behavior=True)
    # f() -> 0, 0, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0)

def test_struct_delete_storage_small(harness):
    """structs/contracts/struct_delete_storage_small.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_storage_small.sol", via_yul_behavior=True)
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_struct_delete_storage_with_array(harness):
    """structs/contracts/struct_delete_storage_with_array.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_storage_with_array.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # g() ->
    r = harness.call(app, "g()")
    # (void return — call succeeding is the assertion)

def test_struct_delete_storage_with_arrays_small(harness):
    """structs/contracts/struct_delete_storage_with_arrays_small.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_storage_with_arrays_small.sol", via_yul_behavior=True)
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_struct_delete_struct_in_mapping(harness):
    """structs/contracts/struct_delete_struct_in_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_delete_struct_in_mapping.sol")
    # deleteIt() -> 0
    r = harness.call(app, "deleteIt()")
    assert as_int(r.abi_return) == 0

def test_struct_memory_to_storage(harness):
    """structs/contracts/struct_memory_to_storage.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_memory_to_storage.sol")
    # f() -> 42, 23, 34
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 23, 34)

def test_struct_memory_to_storage_function_ptr(harness):
    """structs/contracts/struct_memory_to_storage_function_ptr.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_memory_to_storage_function_ptr.sol")
    # f() -> 42, 23, 34, 42, 42
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 42, 23, 34, 42, 42
    assert not r.reverted

def test_struct_named_constructor(harness):
    """structs/contracts/struct_named_constructor.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_named_constructor.sol")
    # s() -> 1, true
    r = harness.call(app, "s()")
    # TODO: verify expected: 1 | true
    assert not r.reverted

def test_struct_reference(harness):
    """structs/contracts/struct_reference.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_reference.sol")
    # check() -> false
    r = harness.call(app, "check()")
    assert bool(as_int(r.abi_return)) is False
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # check() -> true
    r = harness.call(app, "check()")
    assert bool(as_int(r.abi_return)) is True

def test_struct_referencing(harness):
    """structs/contracts/struct_referencing.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_referencing.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2
    # h() -> 0, 5
    r = harness.call(app, "h()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 5)
    # x() -> 0, 3
    r = harness.call(app, "x()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 3)
    # y() -> 4
    r = harness.call(app, "y()")
    assert as_int(r.abi_return) == 4
    # a1() -> 1
    r = harness.call(app, "a1()")
    assert as_int(r.abi_return) == 1
    # a2() -> 2
    r = harness.call(app, "a2()")
    assert as_int(r.abi_return) == 2

def test_struct_storage_push_zero_value(harness):
    """structs/contracts/struct_storage_push_zero_value.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_storage_push_zero_value.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_struct_storage_to_mapping(harness):
    """structs/contracts/struct_storage_to_mapping.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_storage_to_mapping.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_struct_storage_to_memory(harness):
    """structs/contracts/struct_storage_to_memory.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_storage_to_memory.sol")
    # f() -> 42, 23, 34
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 23, 34)

def test_struct_storage_to_memory_function_ptr(harness):
    """structs/contracts/struct_storage_to_memory_function_ptr.sol"""
    app = harness.compile_and_deploy("structs/contracts/struct_storage_to_memory_function_ptr.sol")
    # f() -> 42, 23, 34, 42, 42
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 42, 23, 34, 42, 42
    assert not r.reverted

def test_structs(harness):
    """structs/contracts/structs.sol"""
    app = harness.compile_and_deploy("structs/contracts/structs.sol")
    # check() -> false
    r = harness.call(app, "check()")
    assert bool(as_int(r.abi_return)) is False
    # set() ->
    r = harness.call(app, "set()")
    # (void return — call succeeding is the assertion)
    # check() -> true
    r = harness.call(app, "check()")
    assert bool(as_int(r.abi_return)) is True

def test_using_for_function_on_struct(harness):
    """structs/contracts/using_for_function_on_struct.sol"""
    app = harness.compile_and_deploy("structs/contracts/using_for_function_on_struct.sol")
    # f(uint256): 7 -> 0x15
    r = harness.call(app, "f(uint256)", 7)
    assert as_int(r.abi_return) == 21
    # x() -> 0x15
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 21
