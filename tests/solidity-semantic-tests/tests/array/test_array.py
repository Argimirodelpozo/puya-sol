"""Auto-generated tests for the array category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_array_2d_assignment(harness):
    """array/array_2d_assignment.sol"""
    app = harness.compile_and_deploy("array/array_2d_assignment.sol")
    # f(uint256): 42 -> 42
    r = harness.call(app, "f(uint256)", 42)
    assert r.abi_return == 42

def test_array_2d_new(harness):
    """array/array_2d_new.sol"""
    app = harness.compile_and_deploy("array/array_2d_new.sol")
    # f(uint256): 42 -> 42
    r = harness.call(app, "f(uint256)", 42)
    assert r.abi_return == 42

def test_array_3d_assignment(harness):
    """array/array_3d_assignment.sol"""
    app = harness.compile_and_deploy("array/array_3d_assignment.sol")
    # f(uint256): 42 -> 42
    r = harness.call(app, "f(uint256)", 42)
    assert r.abi_return == 42

def test_array_3d_new(harness):
    """array/array_3d_new.sol"""
    app = harness.compile_and_deploy("array/array_3d_new.sol")
    # f(uint256): 42 -> 42
    r = harness.call(app, "f(uint256)", 42)
    assert r.abi_return == 42

def test_array_function_pointers(harness):
    """array/array_function_pointers.sol"""
    app = harness.compile_and_deploy("array/array_function_pointers.sol")
    # f(uint256,uint256): 1823621, 12323 -> FAILURE # Out of gas #
    r = harness.call(app, "f(uint256,uint256)", 1823621, 12323, expect_revert=True)
    assert r.reverted
    # f2(uint256,uint256,uint256,uint256): 18723921, 1823621, 123, 12323 -> FAILURE # Out of gas #
    r = harness.call(app, "f2(uint256,uint256,uint256,uint256)", 18723921, 1823621, 123, 12323, expect_revert=True)
    assert r.reverted
    # g(uint256,uint256): 1823621, 12323 -> FAILURE # Out of gas #
    r = harness.call(app, "g(uint256,uint256)", 1823621, 12323, expect_revert=True)
    assert r.reverted
    # g2(uint256,uint256,uint256,uint256): 18723921, 1823621, 123, 12323 -> FAILURE # Out of gas #
    r = harness.call(app, "g2(uint256,uint256,uint256,uint256)", 18723921, 1823621, 123, 12323, expect_revert=True)
    assert r.reverted

def test_array_memory_as_parameter(harness):
    """array/array_memory_as_parameter.sol"""
    app = harness.compile_and_deploy("array/array_memory_as_parameter.sol")
    # test(uint256,uint256): 0, 0 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # test(uint256,uint256): 1, 0 -> 1
    r = harness.call(app, "test(uint256,uint256)", 1, 0)
    assert r.abi_return == 1
    # test(uint256,uint256): 10, 5 -> 6
    r = harness.call(app, "test(uint256,uint256)", 10, 5)
    assert r.abi_return == 6
    # test(uint256,uint256): 10, 50 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256,uint256)", 10, 50, expect_revert=True)
    assert r.reverted

def test_array_memory_create(harness):
    """array/array_memory_create.sol"""
    app = harness.compile_and_deploy("array/array_memory_create.sol")
    # create(uint256): 0 -> 0
    r = harness.call(app, "create(uint256)", 0)
    assert r.abi_return == 0
    # create(uint256): 7 -> 7
    r = harness.call(app, "create(uint256)", 7)
    assert r.abi_return == 7
    # create(uint256): 10 -> 10
    r = harness.call(app, "create(uint256)", 10)
    assert r.abi_return == 10

def test_array_memory_index_access(harness):
    """array/array_memory_index_access.sol"""
    app = harness.compile_and_deploy("array/array_memory_index_access.sol")
    # index(uint256): 0 -> true
    r = harness.call(app, "index(uint256)", 0)
    assert r.abi_return is True
    # index(uint256): 10 -> true
    r = harness.call(app, "index(uint256)", 10)
    assert r.abi_return is True
    # index(uint256): 20 -> true
    r = harness.call(app, "index(uint256)", 20)
    assert r.abi_return is True
    # index(uint256): 0xFF -> true
    r = harness.call(app, "index(uint256)", 255)
    assert r.abi_return is True
    # accessIndex(uint256,int256): 10, 1 -> 2
    r = harness.call(app, "accessIndex(uint256,int256)", 10, 1)
    assert r.abi_return == 2
    # accessIndex(uint256,int256): 10, 0 -> 1
    r = harness.call(app, "accessIndex(uint256,int256)", 10, 0)
    assert r.abi_return == 1
    # accessIndex(uint256,int256): 10, 11 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "accessIndex(uint256,int256)", 10, 11, expect_revert=True)
    assert r.reverted
    # accessIndex(uint256,int256): 10, 10 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "accessIndex(uint256,int256)", 10, 10, expect_revert=True)
    assert r.reverted
    # accessIndex(uint256,int256): 10, -1 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "accessIndex(uint256,int256)", 10, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted

def test_array_push_return_reference(harness):
    """array/array_push_return_reference.sol"""
    app = harness.compile_and_deploy("array/array_push_return_reference.sol")
    # getLength() -> 0
    r = harness.call(app, "getLength()")
    assert r.abi_return == 0
    # test(uint256): 42 ->
    r = harness.call(app, "test(uint256)", 42)
    # (void return — call succeeding is the assertion)
    # getLength() -> 1
    r = harness.call(app, "getLength()")
    assert r.abi_return == 1
    # fetch(uint256): 0 -> 42
    r = harness.call(app, "fetch(uint256)", 0)
    assert r.abi_return == 42
    # fetch(uint256): 1 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "fetch(uint256)", 1, expect_revert=True)
    assert r.reverted
    # test(uint256): 23 ->
    r = harness.call(app, "test(uint256)", 23)
    # (void return — call succeeding is the assertion)
    # getLength() -> 2
    r = harness.call(app, "getLength()")
    assert r.abi_return == 2
    # fetch(uint256): 0 -> 42
    r = harness.call(app, "fetch(uint256)", 0)
    assert r.abi_return == 42
    # fetch(uint256): 1 -> 23
    r = harness.call(app, "fetch(uint256)", 1)
    assert r.abi_return == 23
    # fetch(uint256): 2 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "fetch(uint256)", 2, expect_revert=True)
    assert r.reverted

def test_array_push_with_arg(harness):
    """array/array_push_with_arg.sol"""
    app = harness.compile_and_deploy("array/array_push_with_arg.sol")
    # getLength() -> 0
    r = harness.call(app, "getLength()")
    assert r.abi_return == 0
    # test(uint256): 42 ->
    r = harness.call(app, "test(uint256)", 42)
    # (void return — call succeeding is the assertion)
    # getLength() -> 1
    r = harness.call(app, "getLength()")
    assert r.abi_return == 1
    # fetch(uint256): 0 -> 42
    r = harness.call(app, "fetch(uint256)", 0)
    assert r.abi_return == 42
    # fetch(uint256): 1 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "fetch(uint256)", 1, expect_revert=True)
    assert r.reverted
    # test(uint256): 23 ->
    r = harness.call(app, "test(uint256)", 23)
    # (void return — call succeeding is the assertion)
    # getLength() -> 2
    r = harness.call(app, "getLength()")
    assert r.abi_return == 2
    # fetch(uint256): 0 -> 42
    r = harness.call(app, "fetch(uint256)", 0)
    assert r.abi_return == 42
    # fetch(uint256): 1 -> 23
    r = harness.call(app, "fetch(uint256)", 1)
    assert r.abi_return == 23
    # fetch(uint256): 2 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "fetch(uint256)", 2, expect_revert=True)
    assert r.reverted

def test_array_storage_index_access(harness):
    """array/array_storage_index_access.sol"""
    app = harness.compile_and_deploy("array/array_storage_index_access.sol")
    # test_indices(uint256): 1 ->
    r = harness.call(app, "test_indices(uint256)", 1)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 129 ->
    r = harness.call(app, "test_indices(uint256)", 129)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 5 ->
    r = harness.call(app, "test_indices(uint256)", 5)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 10 ->
    r = harness.call(app, "test_indices(uint256)", 10)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 15 ->
    r = harness.call(app, "test_indices(uint256)", 15)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 0xFF ->
    r = harness.call(app, "test_indices(uint256)", 255)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 1000 ->
    r = harness.call(app, "test_indices(uint256)", 1000)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 129 ->
    r = harness.call(app, "test_indices(uint256)", 129)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 128 ->
    r = harness.call(app, "test_indices(uint256)", 128)
    # (void return — call succeeding is the assertion)
    # test_indices(uint256): 1 ->
    r = harness.call(app, "test_indices(uint256)", 1)
    # (void return — call succeeding is the assertion)

def test_array_storage_index_boundary_test(harness):
    """array/array_storage_index_boundary_test.sol"""
    app = harness.compile_and_deploy("array/array_storage_index_boundary_test.sol")
    # test_boundary_check(uint256,uint256): 10, 11 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 10, 11, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 10, 9 -> 0
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 10, 9)
    assert r.abi_return == 0
    # test_boundary_check(uint256,uint256): 1, 9 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 1, 9, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 1, 1 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 1, 1, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 10, 10 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 10, 10, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 256, 256 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 256, 256, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 256, 255 -> 0
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 256, 255)
    assert r.abi_return == 0
    # test_boundary_check(uint256,uint256): 256, 0xFFFF -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 256, 65535, expect_revert=True)
    assert r.reverted
    # test_boundary_check(uint256,uint256): 256, 2 -> 0
    r = harness.call(app, "test_boundary_check(uint256,uint256)", 256, 2)
    assert r.abi_return == 0

def test_array_storage_index_zeroed_test(harness):
    """array/array_storage_index_zeroed_test.sol"""
    app = harness.compile_and_deploy("array/array_storage_index_zeroed_test.sol")
    # test_zeroed_indices(uint256): 1 ->
    r = harness.call(app, "test_zeroed_indices(uint256)", 1)
    # (void return — call succeeding is the assertion)
    # test_zeroed_indices(uint256): 5 ->
    r = harness.call(app, "test_zeroed_indices(uint256)", 5)
    # (void return — call succeeding is the assertion)
    # test_zeroed_indices(uint256): 10 ->
    r = harness.call(app, "test_zeroed_indices(uint256)", 10)
    # (void return — call succeeding is the assertion)
    # test_zeroed_indices(uint256): 15 ->
    r = harness.call(app, "test_zeroed_indices(uint256)", 15)
    # (void return — call succeeding is the assertion)
    # test_zeroed_indices(uint256): 0xFF ->
    r = harness.call(app, "test_zeroed_indices(uint256)", 255)
    # (void return — call succeeding is the assertion)

def test_array_storage_length_access(harness):
    """array/array_storage_length_access.sol"""
    app = harness.compile_and_deploy("array/array_storage_length_access.sol")
    # set_get_length(uint256): 0 -> 0
    r = harness.call(app, "set_get_length(uint256)", 0)
    assert r.abi_return == 0
    # set_get_length(uint256): 1 -> 1
    r = harness.call(app, "set_get_length(uint256)", 1)
    assert r.abi_return == 1
    # set_get_length(uint256): 10 -> 10
    r = harness.call(app, "set_get_length(uint256)", 10)
    assert r.abi_return == 10
    # set_get_length(uint256): 20 -> 20
    r = harness.call(app, "set_get_length(uint256)", 20)
    assert r.abi_return == 20
    # set_get_length(uint256): 0xFF -> 0xFF
    r = harness.call(app, "set_get_length(uint256)", 255)
    assert r.abi_return == 255
    # set_get_length(uint256): 0xFFF -> 0xFFF
    r = harness.call(app, "set_get_length(uint256)", 4095)
    assert r.abi_return == 4095
    # set_get_length(uint256): 0xFFFFF -> FAILURE # Out-of-gas #
    r = harness.call(app, "set_get_length(uint256)", 1048575, expect_revert=True)
    assert r.reverted

def test_array_storage_pop_zero_length(harness):
    """array/array_storage_pop_zero_length.sol"""
    app = harness.compile_and_deploy("array/array_storage_pop_zero_length.sol")
    # popEmpty() -> FAILURE, hex"4e487b71", 0x31
    r = harness.call(app, "popEmpty()", expect_revert=True)
    assert r.reverted

def test_array_storage_push_empty(harness):
    """array/array_storage_push_empty.sol"""
    app = harness.compile_and_deploy("array/array_storage_push_empty.sol")
    # pushEmpty(uint256): 128
    r = harness.call(app, "pushEmpty(uint256)", 128)
    # (void return — call succeeding is the assertion)
    # pushEmpty(uint256): 256
    r = harness.call(app, "pushEmpty(uint256)", 256)
    # (void return — call succeeding is the assertion)
    # pushEmpty(uint256): 38869 -> FAILURE # out-of-gas #
    r = harness.call(app, "pushEmpty(uint256)", 38869, expect_revert=True)
    assert r.reverted

def test_array_storage_push_empty_length_address(harness):
    """array/array_storage_push_empty_length_address.sol"""
    app = harness.compile_and_deploy("array/array_storage_push_empty_length_address.sol")
    # set_get_length(uint256): 0 -> 0
    r = harness.call(app, "set_get_length(uint256)", 0)
    assert r.abi_return == 0
    # set_get_length(uint256): 1 -> 1
    r = harness.call(app, "set_get_length(uint256)", 1)
    assert r.abi_return == 1
    # set_get_length(uint256): 10 -> 10
    r = harness.call(app, "set_get_length(uint256)", 10)
    assert r.abi_return == 10
    # set_get_length(uint256): 20 -> 20
    r = harness.call(app, "set_get_length(uint256)", 20)
    assert r.abi_return == 20
    # set_get_length(uint256): 0 -> 0
    r = harness.call(app, "set_get_length(uint256)", 0)
    assert r.abi_return == 0
    # set_get_length(uint256): 0xFF -> 0xFF
    r = harness.call(app, "set_get_length(uint256)", 255)
    assert r.abi_return == 255
    # set_get_length(uint256): 0xFFF -> 0xFFF
    r = harness.call(app, "set_get_length(uint256)", 4095)
    assert r.abi_return == 4095
    # set_get_length(uint256): 0xFFFFF -> FAILURE # Out-of-gas #
    r = harness.call(app, "set_get_length(uint256)", 1048575, expect_revert=True)
    assert r.reverted

def test_array_storage_push_pop(harness):
    """array/array_storage_push_pop.sol"""
    app = harness.compile_and_deploy("array/array_storage_push_pop.sol")
    # set_get_length(uint256): 0 -> 0
    r = harness.call(app, "set_get_length(uint256)", 0)
    assert r.abi_return == 0
    # set_get_length(uint256): 1 -> 0
    r = harness.call(app, "set_get_length(uint256)", 1)
    assert r.abi_return == 0
    # set_get_length(uint256): 10 -> 0
    r = harness.call(app, "set_get_length(uint256)", 10)
    assert r.abi_return == 0
    # set_get_length(uint256): 20 -> 0
    r = harness.call(app, "set_get_length(uint256)", 20)
    assert r.abi_return == 0
    # set_get_length(uint256): 0xFF -> 0
    r = harness.call(app, "set_get_length(uint256)", 255)
    assert r.abi_return == 0
    # set_get_length(uint256): 0xFFF -> 0
    r = harness.call(app, "set_get_length(uint256)", 4095)
    assert r.abi_return == 0
    # set_get_length(uint256): 0xFFFF -> FAILURE # Out-of-gas #
    r = harness.call(app, "set_get_length(uint256)", 65535, expect_revert=True)
    assert r.reverted

def test_arrays_complex_from_and_to_storage(harness):
    """array/arrays_complex_from_and_to_storage.sol"""
    app = harness.compile_and_deploy("array/arrays_complex_from_and_to_storage.sol")
    # set(uint24[3][]): 0x20, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12 -> 0x06
    r = harness.call(app, "set(uint24[3][])", 32, 6, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18)
    assert r.abi_return == 6
    # data(uint256,uint256): 0x02, 0x02 -> 0x09
    r = harness.call(app, "data(uint256,uint256)", 2, 2)
    assert r.abi_return == 9
    # data(uint256,uint256): 0x05, 0x01 -> 0x11
    r = harness.call(app, "data(uint256,uint256)", 5, 1)
    assert r.abi_return == 17
    # data(uint256,uint256): 0x06, 0x00 -> FAILURE
    r = harness.call(app, "data(uint256,uint256)", 6, 0, expect_revert=True)
    assert r.reverted
    # get() -> 0x20, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12
    r = harness.call(app, "get()")
    # TODO: verify structural decoding matches expected: 32, 6, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18
    assert not r.reverted

def test_byte_array_storage_layout(harness):
    """array/byte_array_storage_layout.sol"""
    app = harness.compile_and_deploy("array/byte_array_storage_layout.sol")
    # test_short() -> 1780731860627700044960722568376587075150542249149356309979516913770823710
    r = harness.call(app, "test_short()")
    assert r.abi_return == 1780731860627700044960722568376587075150542249149356309979516913770823710
    # test_long() -> 67
    r = harness.call(app, "test_long()")
    assert r.abi_return == 67
    # test_pop() -> 1780731860627700044960722568376592200742329637303199754547598369979433020
    r = harness.call(app, "test_pop()")
    assert r.abi_return == 1780731860627700044960722568376592200742329637303199754547598369979433020

def test_byte_array_transitional_2(harness):
    """array/byte_array_transitional_2.sol"""
    app = harness.compile_and_deploy("array/byte_array_transitional_2.sol")
    # test() -> 0
    r = harness.call(app, "test()")
    assert r.abi_return == 0

def test_bytes_length_member(harness):
    """array/bytes_length_member.sol"""
    app = harness.compile_and_deploy("array/bytes_length_member.sol")
    # getLength() -> 0
    r = harness.call(app, "getLength()")
    assert r.abi_return == 0
    # set(): 1, 2 -> true
    r = harness.call(app, "set()", 1, 2)
    assert r.abi_return is True
    # getLength() -> 68
    r = harness.call(app, "getLength()")
    assert r.abi_return == 68

def test_bytes_to_fixed_bytes_cleanup(harness):
    """array/bytes_to_fixed_bytes_cleanup.sol"""
    app = harness.compile_and_deploy("array/bytes_to_fixed_bytes_cleanup.sol", via_yul_behavior=True)
    # fromMemory(bytes): 0x20, 16, "abcdefghabcdefgh" -> "abcdefghabcdef\0\0"
    r = harness.call(app, "fromMemory(bytes)", 32, 16, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "abcdefghabcdef\0\0"
    assert not r.reverted
    # fromCalldata(bytes): 0x20, 15, "abcdefghabcdefgh" -> "abcdefghabcdefg\0"
    r = harness.call(app, "fromCalldata(bytes)", 32, 15, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "abcdefghabcdefg\0"
    assert not r.reverted
    # fromStorage() -> "abcdefghabcdefghabcdefghabcdefg\0"
    r = harness.call(app, "fromStorage()")
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefg\0"
    assert not r.reverted
    # fromSlice(bytes): 0x20, 15, "abcdefghabcdefgh" -> "abcdef\0\0"
    r = harness.call(app, "fromSlice(bytes)", 32, 15, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "abcdef\0\0"
    assert not r.reverted

def test_bytes_to_fixed_bytes_simple(harness):
    """array/bytes_to_fixed_bytes_simple.sol"""
    app = harness.compile_and_deploy("array/bytes_to_fixed_bytes_simple.sol")
    # fromMemory(bytes): 0x20, 16, "abcdefghabcdefgh" -> "abcdefghabcdefgh"
    r = harness.call(app, "fromMemory(bytes)", 32, 16, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "abcdefghabcdefgh"
    assert not r.reverted
    # fromCalldata(bytes): 0x20, 16, "abcdefghabcdefgh" -> "abcdefghabcdefgh"
    r = harness.call(app, "fromCalldata(bytes)", 32, 16, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "abcdefghabcdefgh"
    assert not r.reverted
    # fromStorage() -> "abcdefghabcdefgh"
    r = harness.call(app, "fromStorage()")
    # TODO: verify expected: "abcdefghabcdefgh"
    assert not r.reverted
    # fromStorageLong() -> "abcdefghabcdefghabcdefghabcdefgh"
    r = harness.call(app, "fromStorageLong()")
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefgh"
    assert not r.reverted
    # fromSlice(bytes): 0x20, 16, "abcdefghabcdefgh" -> "bcdefgha"
    r = harness.call(app, "fromSlice(bytes)", 32, 16, bytes.fromhex('61626364656667686162636465666768'))
    # TODO: verify expected: "bcdefgha"
    assert not r.reverted

def test_bytes_to_fixed_bytes_too_long(harness):
    """array/bytes_to_fixed_bytes_too_long.sol"""
    app = harness.compile_and_deploy("array/bytes_to_fixed_bytes_too_long.sol")
    # fromMemory(bytes): 0x20, 33, "abcdefghabcdefghabcdefghabcdefgh", "a" -> "abcdefghabcdefghabcdefghabcdefgh"
    r = harness.call(app, "fromMemory(bytes)", 32, 33, bytes.fromhex('6162636465666768616263646566676861626364656667686162636465666768'), bytes.fromhex('61'))
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefgh"
    assert not r.reverted
    # fromCalldata(bytes): 0x20, 33, "abcdefghabcdefghabcdefghabcdefgh", "a" -> "abcdefghabcdefghabcdefghabcdefgh"
    r = harness.call(app, "fromCalldata(bytes)", 32, 33, bytes.fromhex('6162636465666768616263646566676861626364656667686162636465666768'), bytes.fromhex('61'))
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefgh"
    assert not r.reverted
    # fromStorage() -> "abcdefghabcdefghabcdefghabcdefgh"
    r = harness.call(app, "fromStorage()")
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefgh"
    assert not r.reverted
    # fromSlice(bytes): 0x20, 33, "abcdefghabcdefghabcdefghabcdefgh", "a" -> "abcdefghabcdefghabcdefghabcdefgh"
    r = harness.call(app, "fromSlice(bytes)", 32, 33, bytes.fromhex('6162636465666768616263646566676861626364656667686162636465666768'), bytes.fromhex('61'))
    # TODO: verify expected: "abcdefghabcdefghabcdefghabcdefgh"
    assert not r.reverted

def test_calldata_array(harness):
    """array/calldata_array.sol"""
    app = harness.compile_and_deploy("array/calldata_array.sol")
    # f(uint256[2]): 42, 23 -> 42, 23
    r = harness.call(app, "f(uint256[2])", 42, 23)
    assert tuple(r.abi_return) == (42, 23)

def test_calldata_array_as_argument_internal_function(harness):
    """array/calldata_array_as_argument_internal_function.sol"""
    app = harness.compile_and_deploy("array/calldata_array_as_argument_internal_function.sol")
    # g(uint256[]): 0x20, 4, 1, 2, 3, 4 -> 4, 1
    r = harness.call(app, "g(uint256[])", 32, 4, 1, 2, 3, 4)
    assert tuple(r.abi_return) == (4, 1)
    # h(uint256[],uint256,uint256): 0x60, 1, 3, 4, 1, 2, 3, 4 -> 2, 2
    r = harness.call(app, "h(uint256[],uint256,uint256)", 96, 1, 3, 4, 1, 2, 3, 4)
    assert tuple(r.abi_return) == (2, 2)

def test_calldata_array_dynamic_invalid(harness):
    """array/calldata_array_dynamic_invalid.sol"""
    app = harness.compile_and_deploy("array/calldata_array_dynamic_invalid.sol")
    # f(uint256[][]): 0x20, 0x0 -> 42 # valid access stub #
    r = harness.call(app, "f(uint256[][])", 32, 0)
    # TODO: verify expected: 42 # valid access stub #
    assert not r.reverted
    # f(uint256[][]): 0x20, 0x1 -> FAILURE # invalid on argument decoding #
    r = harness.call(app, "f(uint256[][])", 32, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[][]): 0x20, 0x1, 0x20 -> 42 # invalid on outer access #
    r = harness.call(app, "f(uint256[][])", 32, 1, 32)
    # TODO: verify expected: 42 # invalid on outer access #
    assert not r.reverted
    # g(uint256[][]): 0x20, 0x1, 0x20 -> FAILURE
    r = harness.call(app, "g(uint256[][])", 32, 1, 32, expect_revert=True)
    assert r.reverted
    # f(uint256[][]): 0x20, 0x1, 0x20, 0x2, 0x42 -> 42 # invalid on inner access #
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 2, 66)
    # TODO: verify expected: 42 # invalid on inner access #
    assert not r.reverted
    # g(uint256[][]): 0x20, 0x1, 0x20, 0x2, 0x42 -> FAILURE
    r = harness.call(app, "g(uint256[][])", 32, 1, 32, 2, 66, expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_invalid_static_middle(harness):
    """array/calldata_array_dynamic_invalid_static_middle.sol"""
    app = harness.compile_and_deploy("array/calldata_array_dynamic_invalid_static_middle.sol")
    # f(uint256[][1][]): 0x20, 0x0 -> 42 # valid access stub #
    r = harness.call(app, "f(uint256[][1][])", 32, 0)
    # TODO: verify expected: 42 # valid access stub #
    assert not r.reverted
    # f(uint256[][1][]): 0x20, 0x1 -> FAILURE # invalid on argument decoding #
    r = harness.call(app, "f(uint256[][1][])", 32, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[][1][]): 0x20, 0x1, 0x20 -> 42 # invalid on outer access #
    r = harness.call(app, "f(uint256[][1][])", 32, 1, 32)
    # TODO: verify expected: 42 # invalid on outer access #
    assert not r.reverted
    # g(uint256[][1][]): 0x20, 0x1, 0x20 -> FAILURE
    r = harness.call(app, "g(uint256[][1][])", 32, 1, 32, expect_revert=True)
    assert r.reverted
    # f(uint256[][1][]): 0x20, 0x1, 0x20, 0x20 -> 42 # invalid on inner access #
    r = harness.call(app, "f(uint256[][1][])", 32, 1, 32, 32)
    # TODO: verify expected: 42 # invalid on inner access #
    assert not r.reverted
    # g(uint256[][1][]): 0x20, 0x1, 0x20, 0x20 -> 42
    r = harness.call(app, "g(uint256[][1][])", 32, 1, 32, 32)
    assert r.abi_return == 42
    # h(uint256[][1][]): 0x20, 0x1, 0x20, 0x20 -> FAILURE
    r = harness.call(app, "h(uint256[][1][])", 32, 1, 32, 32, expect_revert=True)
    assert r.reverted
    # f(uint256[][1][]): 0x20, 0x1, 0x20, 0x20, 0x1 -> 42
    r = harness.call(app, "f(uint256[][1][])", 32, 1, 32, 32, 1)
    assert r.abi_return == 42
    # g(uint256[][1][]): 0x20, 0x1, 0x20, 0x20, 0x1 -> 42
    r = harness.call(app, "g(uint256[][1][])", 32, 1, 32, 32, 1)
    assert r.abi_return == 42
    # h(uint256[][1][]): 0x20, 0x1, 0x20, 0x20, 0x1 -> FAILURE
    r = harness.call(app, "h(uint256[][1][])", 32, 1, 32, 32, 1, expect_revert=True)
    assert r.reverted

def test_calldata_array_of_struct(harness):
    """array/calldata_array_of_struct.sol"""
    app = harness.compile_and_deploy("array/calldata_array_of_struct.sol")
    # f((uint256,uint256)[]): 0x20, 0x2, 0x1, 0x2, 0x3, 0x4 -> 2, 1, 2, 3, 4
    r = harness.call(app, "f((uint256,uint256)[])", 32, 2, 1, 2, 3, 4)
    # TODO: verify structural decoding matches expected: 2, 1, 2, 3, 4
    assert not r.reverted

def test_calldata_array_two_dimensional(harness):
    """array/calldata_array_two_dimensional.sol"""
    app = harness.compile_and_deploy("array/calldata_array_two_dimensional.sol")
    # test(uint256[][2]): 0x20, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 2
    r = harness.call(app, "test(uint256[][2])", 32, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2
    # test(uint256[][2],uint256): 0x40, 0, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 3
    r = harness.call(app, "test(uint256[][2],uint256)", 64, 0, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 3
    # test(uint256[][2],uint256): 0x40, 1, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 4
    r = harness.call(app, "test(uint256[][2],uint256)", 64, 1, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 4
    # test(uint256[][2],uint256,uint256): 0x60, 0, 0, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A01
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 0, 0, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2561
    # reenc(uint256[][2],uint256,uint256): 0x60, 0, 0, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A01
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 0, 0, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2561
    # test(uint256[][2],uint256,uint256): 0x60, 0, 1, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A02
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 0, 1, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2562
    # reenc(uint256[][2],uint256,uint256): 0x60, 0, 1, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A02
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 0, 1, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2562
    # test(uint256[][2],uint256,uint256): 0x60, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A03
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2563
    # reenc(uint256[][2],uint256,uint256): 0x60, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A03
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2563
    # test(uint256[][2],uint256,uint256): 0x60, 1, 0, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B01
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 1, 0, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2817
    # reenc(uint256[][2],uint256,uint256): 0x60, 1, 0, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B01
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 1, 0, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2817
    # test(uint256[][2],uint256,uint256): 0x60, 1, 1, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B02
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 1, 1, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2818
    # reenc(uint256[][2],uint256,uint256): 0x60, 1, 1, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B02
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 1, 1, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2818
    # test(uint256[][2],uint256,uint256): 0x60, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B03
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2819
    # reenc(uint256[][2],uint256,uint256): 0x60, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B03
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2819
    # test(uint256[][2],uint256,uint256): 0x60, 1, 3, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B04
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 1, 3, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2820
    # reenc(uint256[][2],uint256,uint256): 0x60, 1, 3, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B04
    r = harness.call(app, "reenc(uint256[][2],uint256,uint256)", 96, 1, 3, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2820
    # test(uint256[][2],uint256,uint256): 0x60, 0, 3, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 0, 3, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted
    # test(uint256[][2],uint256,uint256): 0x60, 1, 4, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][2],uint256,uint256)", 96, 1, 4, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted
    # test(uint256[][2],uint256): 0x40, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][2],uint256)", 64, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted

def test_calldata_array_two_dimensional_1(harness):
    """array/calldata_array_two_dimensional_1.sol"""
    app = harness.compile_and_deploy("array/calldata_array_two_dimensional_1.sol")
    # test(uint256[][]): 0x20, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 2
    r = harness.call(app, "test(uint256[][])", 32, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2
    # test(uint256[][],uint256): 0x40, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 3
    r = harness.call(app, "test(uint256[][],uint256)", 64, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 3
    # test(uint256[][],uint256): 0x40, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 4
    r = harness.call(app, "test(uint256[][],uint256)", 64, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 4
    # test(uint256[][],uint256,uint256): 0x60, 0, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A01
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 0, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2561
    # reenc(uint256[][],uint256,uint256): 0x60, 0, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A01
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 0, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2561
    # test(uint256[][],uint256,uint256): 0x60, 0, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A02
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 0, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2562
    # reenc(uint256[][],uint256,uint256): 0x60, 0, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A02
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 0, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2562
    # test(uint256[][],uint256,uint256): 0x60, 0, 2, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A03
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 0, 2, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2563
    # reenc(uint256[][],uint256,uint256): 0x60, 0, 2, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0A03
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 0, 2, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2563
    # test(uint256[][],uint256,uint256): 0x60, 1, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B01
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 1, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2817
    # reenc(uint256[][],uint256,uint256): 0x60, 1, 0, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B01
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 1, 0, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2817
    # test(uint256[][],uint256,uint256): 0x60, 1, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B02
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 1, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2818
    # reenc(uint256[][],uint256,uint256): 0x60, 1, 1, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B02
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 1, 1, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2818
    # test(uint256[][],uint256,uint256): 0x60, 1, 2, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B03
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 1, 2, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2819
    # reenc(uint256[][],uint256,uint256): 0x60, 1, 2, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B03
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 1, 2, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2819
    # test(uint256[][],uint256,uint256): 0x60, 1, 3, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B04
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 1, 3, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2820
    # reenc(uint256[][],uint256,uint256): 0x60, 1, 3, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> 0x0B04
    r = harness.call(app, "reenc(uint256[][],uint256,uint256)", 96, 1, 3, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    assert r.abi_return == 2820
    # test(uint256[][],uint256,uint256): 0x60, 0, 3, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 0, 3, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted
    # test(uint256[][],uint256,uint256): 0x60, 1, 4, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][],uint256,uint256)", 96, 1, 4, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted
    # test(uint256[][],uint256): 0x40, 2, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "test(uint256[][],uint256)", 64, 2, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820, expect_revert=True)
    assert r.reverted

def test_calldata_bytes_array_bounds(harness):
    """array/calldata_bytes_array_bounds.sol"""
    app = harness.compile_and_deploy("array/calldata_bytes_array_bounds.sol")
    # f(bytes[],uint256): 0x40, 0, 1, 0x20, 2, 0x6162000000000000000000000000000000000000000000000000000000000000 -> 0x61
    r = harness.call(app, "f(bytes[],uint256)", 64, 0, 1, 32, 2, 0x6162000000000000000000000000000000000000000000000000000000000000)
    assert r.abi_return == 97
    # f(bytes[],uint256): 0x40, 1, 1, 0x20, 2, 0x6162000000000000000000000000000000000000000000000000000000000000 -> 0x62
    r = harness.call(app, "f(bytes[],uint256)", 64, 1, 1, 32, 2, 0x6162000000000000000000000000000000000000000000000000000000000000)
    assert r.abi_return == 98
    # f(bytes[],uint256): 0x40, 2, 1, 0x20, 2, 0x6162000000000000000000000000000000000000000000000000000000000000 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f(bytes[],uint256)", 64, 2, 1, 32, 2, 0x6162000000000000000000000000000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted

def test_calldata_slice_access(harness):
    """array/calldata_slice_access.sol"""
    app = harness.compile_and_deploy("array/calldata_slice_access.sol")
    # f(uint256[],uint256,uint256): 0x80, 0, 0, 0, 1, 42 ->
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0, 0, 0, 1, 42)
    # (void return — call succeeding is the assertion)
    # f(uint256[],uint256,uint256): 0x80, 0, 1, 0, 1, 42 ->
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0, 1, 0, 1, 42)
    # (void return — call succeeding is the assertion)
    # f(uint256[],uint256,uint256): 0x80, 0, 2, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0, 2, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 1, 0, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 1, 0, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 1, 1, 0, 1, 42 ->
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 1, 1, 0, 1, 42)
    # (void return — call succeeding is the assertion)
    # f(uint256[],uint256,uint256): 0x80, 1, 2, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 1, 2, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 2, 0, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 2, 0, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 2, 1, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 2, 1, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 2, 2, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 2, 2, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 0, 2, 1, 0, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0, 2, 1, 0, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, 1, 2, 0, 2, 42, 23 ->
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 1, 2, 0, 2, 42, 23)
    # (void return — call succeeding is the assertion)
    # f(uint256[],uint256,uint256): 0x80, 1, 3, 0, 2, 42, 23 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 1, 3, 0, 2, 42, 23, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, -1, 0, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x80, -1, -1, 0, 1, 42 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 128, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 0, 1, 42 -> 42, 42, 42
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 0, 1, 42)
    assert tuple(r.abi_return) == (42, 42, 42)
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 1, 1, 42 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 1, 1, 42, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 0, 0, 1, 42 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 0, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 1, 0, 1, 42 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 1, 0, 1, 42, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 5, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4201, 0x4201, 0x4201
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 5, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16897, 16897, 16897)
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 5, 4, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4205, 0x4205, 0x4205
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 5, 4, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16901, 16901, 16901)
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 5, 5, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 5, 5, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 5, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4202, 0x4202, 0x4202
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 5, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16898, 16898, 16898)
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 5, 3, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4205, 0x4205, 0x4205
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 5, 3, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16901, 16901, 16901)
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 5, 4, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 5, 4, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 4, 5, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4205, 0x4205, 0x4205
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 4, 5, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16901, 16901, 16901)
    # g(uint256[],uint256,uint256,uint256): 0x80, 4, 5, 1, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 4, 5, 1, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 5, 5, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 5, 5, 0, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4201, 0x4201, 0x4201
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16897, 16897, 16897)
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 1, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 1, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4201, 0x4201, 0x4201
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16897, 16897, 16897)
    # g(uint256[],uint256,uint256,uint256): 0x80, 0, 1, 1, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 0, 1, 1, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 2, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4202, 0x4202, 0x4202
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 2, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16898, 16898, 16898)
    # g(uint256[],uint256,uint256,uint256): 0x80, 1, 2, 1, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 1, 2, 1, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 4, 5, 0, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> 0x4205, 0x4205, 0x4205
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 4, 5, 0, 5, 16897, 16898, 16899, 16900, 16901)
    assert tuple(r.abi_return) == (16901, 16901, 16901)
    # g(uint256[],uint256,uint256,uint256): 0x80, 4, 5, 1, 5, 0x4201, 0x4202, 0x4203, 0x4204, 0x4205 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 4, 5, 1, 5, 16897, 16898, 16899, 16900, 16901, expect_revert=True)
    assert r.reverted

def test_constant_var_as_array_length(harness):
    """array/constant_var_as_array_length.sol"""
    app = harness.compile_and_deploy("array/constant_var_as_array_length.sol", ctor_args=[1, 2, 3])
    # a(uint256): 0 -> 1
    r = harness.call(app, "a(uint256)", 0)
    assert r.abi_return == 1
    # a(uint256): 1 -> 2
    r = harness.call(app, "a(uint256)", 1)
    assert r.abi_return == 2
    # a(uint256): 2 -> 3
    r = harness.call(app, "a(uint256)", 2)
    assert r.abi_return == 3

def test_create_dynamic_array_with_zero_length(harness):
    """array/create_dynamic_array_with_zero_length.sol"""
    app = harness.compile_and_deploy("array/create_dynamic_array_with_zero_length.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7

def test_create_memory_array(harness):
    """array/create_memory_array.sol"""
    app = harness.compile_and_deploy("array/create_memory_array.sol")
    # f() -> "A", 8, 4, "B"
    r = harness.call(app, "f()")
    # TODO: verify expected: "A" | 8 | 4 | "B"
    assert not r.reverted

def test_create_memory_array_too_large(harness):
    """array/create_memory_array_too_large.sol"""
    app = harness.compile_and_deploy("array/create_memory_array_too_large.sol")
    # f() -> FAILURE, hex"4e487b71", 0x41
    r = harness.call(app, "f()", expect_revert=True)
    assert r.reverted
    # g() -> FAILURE, hex"4e487b71", 0x41
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_create_memory_byte_array(harness):
    """array/create_memory_byte_array.sol"""
    app = harness.compile_and_deploy("array/create_memory_byte_array.sol")
    # f() -> "A"
    r = harness.call(app, "f()")
    # TODO: verify expected: "A"
    assert not r.reverted

def test_create_multiple_dynamic_arrays(harness):
    """array/create_multiple_dynamic_arrays.sol"""
    app = harness.compile_and_deploy("array/create_multiple_dynamic_arrays.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert r.abi_return == 7

def test_dynamic_array_cleanup(harness):
    """array/dynamic_array_cleanup.sol"""
    app = harness.compile_and_deploy("array/dynamic_array_cleanup.sol")
    # fill() ->
    r = harness.call(app, "fill()")
    # (void return — call succeeding is the assertion)
    # halfClear() ->
    r = harness.call(app, "halfClear()")
    # (void return — call succeeding is the assertion)
    # fullClear() ->
    r = harness.call(app, "fullClear()")
    # (void return — call succeeding is the assertion)

def test_dynamic_arrays_in_storage(harness):
    """array/dynamic_arrays_in_storage.sol"""
    app = harness.compile_and_deploy("array/dynamic_arrays_in_storage.sol")
    # getLengths() -> 0, 0
    r = harness.call(app, "getLengths()")
    assert tuple(r.abi_return) == (0, 0)
    # setLengths(uint256,uint256): 48, 49 ->
    r = harness.call(app, "setLengths(uint256,uint256)", 48, 49)
    # (void return — call succeeding is the assertion)
    # getLengths() -> 48, 49
    r = harness.call(app, "getLengths()")
    assert tuple(r.abi_return) == (48, 49)
    # setIDStatic(uint256): 11 ->
    r = harness.call(app, "setIDStatic(uint256)", 11)
    # (void return — call succeeding is the assertion)
    # getID(uint256): 2 -> 11
    r = harness.call(app, "getID(uint256)", 2)
    assert r.abi_return == 11
    # setID(uint256,uint256): 7, 8 ->
    r = harness.call(app, "setID(uint256,uint256)", 7, 8)
    # (void return — call succeeding is the assertion)
    # getID(uint256): 7 -> 8
    r = harness.call(app, "getID(uint256)", 7)
    assert r.abi_return == 8
    # setData(uint256,uint256,uint256): 7, 8, 9 ->
    r = harness.call(app, "setData(uint256,uint256,uint256)", 7, 8, 9)
    # (void return — call succeeding is the assertion)
    # setData(uint256,uint256,uint256): 8, 10, 11 ->
    r = harness.call(app, "setData(uint256,uint256,uint256)", 8, 10, 11)
    # (void return — call succeeding is the assertion)
    # getData(uint256): 7 -> 8, 9
    r = harness.call(app, "getData(uint256)", 7)
    assert tuple(r.abi_return) == (8, 9)
    # getData(uint256): 8 -> 10, 11
    r = harness.call(app, "getData(uint256)", 8)
    assert tuple(r.abi_return) == (10, 11)

def test_dynamic_multi_array_cleanup(harness):
    """array/dynamic_multi_array_cleanup.sol"""
    app = harness.compile_and_deploy("array/dynamic_multi_array_cleanup.sol")
    # fill() -> 8
    r = harness.call(app, "fill()")
    assert r.abi_return == 8
    # clear() ->
    r = harness.call(app, "clear()")
    # (void return — call succeeding is the assertion)

def test_dynamic_out_of_bounds_array_access(harness):
    """array/dynamic_out_of_bounds_array_access.sol"""
    app = harness.compile_and_deploy("array/dynamic_out_of_bounds_array_access.sol")
    # length() -> 0
    r = harness.call(app, "length()")
    assert r.abi_return == 0
    # get(uint256): 3 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "get(uint256)", 3, expect_revert=True)
    assert r.reverted
    # enlarge(uint256): 4 -> 4
    r = harness.call(app, "enlarge(uint256)", 4)
    assert r.abi_return == 4
    # length() -> 4
    r = harness.call(app, "length()")
    assert r.abi_return == 4
    # set(uint256,uint256): 3, 4 -> true
    r = harness.call(app, "set(uint256,uint256)", 3, 4)
    assert r.abi_return is True
    # get(uint256): 3 -> 4
    r = harness.call(app, "get(uint256)", 3)
    assert r.abi_return == 4
    # length() -> 4
    r = harness.call(app, "length()")
    assert r.abi_return == 4
    # set(uint256,uint256): 4, 8 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "set(uint256,uint256)", 4, 8, expect_revert=True)
    assert r.reverted
    # length() -> 4
    r = harness.call(app, "length()")
    assert r.abi_return == 4

def test_evm_exceptions_out_of_band_access(harness):
    """array/evm_exceptions_out_of_band_access.sol"""
    app = harness.compile_and_deploy("array/evm_exceptions_out_of_band_access.sol")
    # test() -> false
    r = harness.call(app, "test()")
    assert r.abi_return is False
    # testIt() -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "testIt()", expect_revert=True)
    assert r.reverted
    # test() -> false
    r = harness.call(app, "test()")
    assert r.abi_return is False

def test_external_array_args(harness):
    """array/external_array_args.sol"""
    app = harness.compile_and_deploy("array/external_array_args.sol")
    # test(uint256[8],uint256[],uint256[5],uint256,uint256,uint256): 1, 2, 3, 4, 5, 6, 7, 8, 0x220, 21, 22, 23, 24, 25, 0, 1, 2, 3, 11, 12, 13 -> 1, 12, 23
    r = harness.call(app, "test(uint256[8],uint256[],uint256[5],uint256,uint256,uint256)", 1, 2, 3, 4, 5, 6, 7, 8, 544, 21, 22, 23, 24, 25, 0, 1, 2, 3, 11, 12, 13)
    assert tuple(r.abi_return) == (1, 12, 23)

def test_fixed_array_cleanup(harness):
    """array/fixed_array_cleanup.sol"""
    app = harness.compile_and_deploy("array/fixed_array_cleanup.sol")
    # fill() ->
    r = harness.call(app, "fill()")
    # (void return — call succeeding is the assertion)
    # clear() ->
    r = harness.call(app, "clear()")
    # (void return — call succeeding is the assertion)

def test_fixed_arrays_as_return_type(harness):
    """array/fixed_arrays_as_return_type.sol"""
    app = harness.compile_and_deploy("array/fixed_arrays_as_return_type.sol")
    # f() -> 2, 3, 4, 5, 6, 1000, 1001, 1002, 1003, 1004
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 2, 3, 4, 5, 6, 1000, 1001, 1002, 1003, 1004
    assert not r.reverted

def test_fixed_arrays_in_constructors(harness):
    """array/fixed_arrays_in_constructors.sol"""
    app = harness.compile_and_deploy("array/fixed_arrays_in_constructors.sol", ctor_args=[1, 2, 3, 4])
    # r() -> 4
    r = harness.call(app, "r()")
    assert r.abi_return == 4
    # ch() -> 3
    r = harness.call(app, "ch()")
    assert r.abi_return == 3

def test_fixed_arrays_in_storage(harness):
    """array/fixed_arrays_in_storage.sol"""
    app = harness.compile_and_deploy("array/fixed_arrays_in_storage.sol")
    # setIDStatic(uint256): 0xb ->
    r = harness.call(app, "setIDStatic(uint256)", 11)
    # (void return — call succeeding is the assertion)
    # getID(uint256): 0x2 -> 0xb
    r = harness.call(app, "getID(uint256)", 2)
    assert r.abi_return == 11
    # setID(uint256,uint256): 0x7, 0x8 ->
    r = harness.call(app, "setID(uint256,uint256)", 7, 8)
    # (void return — call succeeding is the assertion)
    # getID(uint256): 0x7 -> 0x8
    r = harness.call(app, "getID(uint256)", 7)
    assert r.abi_return == 8
    # setData(uint256,uint256,uint256): 0x7, 0x8, 0x9 ->
    r = harness.call(app, "setData(uint256,uint256,uint256)", 7, 8, 9)
    # (void return — call succeeding is the assertion)
    # setData(uint256,uint256,uint256): 0x8, 0xa, 0xb ->
    r = harness.call(app, "setData(uint256,uint256,uint256)", 8, 10, 11)
    # (void return — call succeeding is the assertion)
    # getData(uint256): 0x7 -> 0x8, 0x9
    r = harness.call(app, "getData(uint256)", 7)
    assert tuple(r.abi_return) == (8, 9)
    # getData(uint256): 0x8 -> 0xa, 0xb
    r = harness.call(app, "getData(uint256)", 8)
    assert tuple(r.abi_return) == (10, 11)
    # getLengths() -> 0x400, 0x403
    r = harness.call(app, "getLengths()")
    assert tuple(r.abi_return) == (1024, 1027)

def test_fixed_bytes_length_access(harness):
    """array/fixed_bytes_length_access.sol"""
    app = harness.compile_and_deploy("array/fixed_bytes_length_access.sol")
    # f(bytes32): "789" -> 32, 16, 8
    r = harness.call(app, "f(bytes32)", bytes.fromhex('373839'))
    assert tuple(r.abi_return) == (32, 16, 8)

def test_fixed_out_of_bounds_array_access(harness):
    """array/fixed_out_of_bounds_array_access.sol"""
    app = harness.compile_and_deploy("array/fixed_out_of_bounds_array_access.sol")
    # length() -> 4
    r = harness.call(app, "length()")
    assert r.abi_return == 4
    # set(uint256,uint256): 3, 4 -> true
    r = harness.call(app, "set(uint256,uint256)", 3, 4)
    assert r.abi_return is True
    # set(uint256,uint256): 4, 5 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "set(uint256,uint256)", 4, 5, expect_revert=True)
    assert r.reverted
    # set(uint256,uint256): 400, 5 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "set(uint256,uint256)", 400, 5, expect_revert=True)
    assert r.reverted
    # get(uint256): 3 -> 4
    r = harness.call(app, "get(uint256)", 3)
    assert r.abi_return == 4
    # get(uint256): 4 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "get(uint256)", 4, expect_revert=True)
    assert r.reverted
    # get(uint256): 400 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "get(uint256)", 400, expect_revert=True)
    assert r.reverted
    # length() -> 4
    r = harness.call(app, "length()")
    assert r.abi_return == 4

def test_function_array_cross_calls(harness):
    """array/function_array_cross_calls.sol"""
    app = harness.compile_and_deploy("array/function_array_cross_calls.sol")
    # test() -> 5, 6, 7
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (5, 6, 7)

def test_function_memory_array(harness):
    """array/function_memory_array.sol"""
    app = harness.compile_and_deploy("array/function_memory_array.sol")
    # test(uint256,uint256): 10, 0 -> 11
    r = harness.call(app, "test(uint256,uint256)", 10, 0)
    assert r.abi_return == 11
    # test(uint256,uint256): 10, 1 -> 12
    r = harness.call(app, "test(uint256,uint256)", 10, 1)
    assert r.abi_return == 12
    # test(uint256,uint256): 10, 2 -> 13
    r = harness.call(app, "test(uint256,uint256)", 10, 2)
    assert r.abi_return == 13
    # test(uint256,uint256): 10, 3 -> 15
    r = harness.call(app, "test(uint256,uint256)", 10, 3)
    assert r.abi_return == 15
    # test(uint256,uint256): 10, 4 -> 18
    r = harness.call(app, "test(uint256,uint256)", 10, 4)
    assert r.abi_return == 18
    # test(uint256,uint256): 10, 5 -> FAILURE, hex"4e487b71", 0x51
    r = harness.call(app, "test(uint256,uint256)", 10, 5, expect_revert=True)
    assert r.reverted

def test_inline_array_return(harness):
    """array/inline_array_return.sol"""
    app = harness.compile_and_deploy("array/inline_array_return.sol")
    # f() -> 1, 2, 3, 4, 5
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5
    assert not r.reverted

def test_inline_array_singleton(harness):
    """array/inline_array_singleton.sol"""
    app = harness.compile_and_deploy("array/inline_array_singleton.sol")
    # f() -> 4
    r = harness.call(app, "f()")
    assert r.abi_return == 4

def test_inline_array_storage_to_memory_conversion_ints(harness):
    """array/inline_array_storage_to_memory_conversion_ints.sol"""
    app = harness.compile_and_deploy("array/inline_array_storage_to_memory_conversion_ints.sol")
    # f() -> 3, 6
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (3, 6)

def test_inline_array_storage_to_memory_conversion_strings(harness):
    """array/inline_array_storage_to_memory_conversion_strings.sol"""
    app = harness.compile_and_deploy("array/inline_array_storage_to_memory_conversion_strings.sol")
    # f() -> 0x40, 0x80, 0x3, "ray", 0x2, "mi"
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x40 | 0x80 | 0x3 | "ray" | 0x2 | "mi"
    assert not r.reverted

def test_inline_array_strings_from_document(harness):
    """array/inline_array_strings_from_document.sol"""
    app = harness.compile_and_deploy("array/inline_array_strings_from_document.sol")
    # f(uint256): 0 -> 0x20, 0x4, "This"
    r = harness.call(app, "f(uint256)", 0)
    assert r.abi_return == 'This'
    # f(uint256): 1 -> 0x20, 0x2, "is"
    r = harness.call(app, "f(uint256)", 1)
    assert r.abi_return == 'is'
    # f(uint256): 2 -> 0x20, 0x2, "an"
    r = harness.call(app, "f(uint256)", 2)
    assert r.abi_return == 'an'
    # f(uint256): 3 -> 0x20, 0x5, "array"
    r = harness.call(app, "f(uint256)", 3)
    assert r.abi_return == 'array'

def test_invalid_encoding_for_storage_byte_array(harness):
    """array/invalid_encoding_for_storage_byte_array.sol"""
    app = harness.compile_and_deploy("array/invalid_encoding_for_storage_byte_array.sol")
    # x() -> 0x20, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)
    # abiEncode() -> 0x20, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "abiEncode()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)
    # abiEncodePacked() -> 0x20, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "abiEncodePacked()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)
    # copyToMemory() -> 0x20, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "copyToMemory()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)
    # indexAccess() -> 0x6100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "indexAccess()")
    assert r.abi_return == 43874346312576839672212443538448152585028080127215369968075725190498334277632
    # arrayPushEmpty()
    r = harness.call(app, "arrayPushEmpty()")
    # (void return — call succeeding is the assertion)
    # arrayPush()
    r = harness.call(app, "arrayPush()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 5, 0x6162630074000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 5, 44048180610029679435879196391309815236404736357889256560704300593623240540160)
    # arrayPop()
    r = harness.call(app, "arrayPop()")
    # (void return — call succeeding is the assertion)
    # assignToLong()
    r = harness.call(app, "assignToLong()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 0x25, 0x3132333435363738393031323334353637383930313233343536373839303132, 0x3334353637000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 37, 22252025330403739761828227648604333229819926301751889444568374711659082559794, 23160198579300737759963791862203182809477568982966167397766552798909578608640)
    # assignTo()
    r = harness.call(app, "assignTo()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 3, 0x6465660000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 3, 45410440389996628292126647411691701031878313646715444222112027556892448391168)
    # copyFromStorageShort()
    r = harness.call(app, "copyFromStorageShort()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 3, 0x6162630000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 3, 44048180597813453602326562734351324025098966208897425494240603688123167145984)
    # copyFromStorageLong()
    r = harness.call(app, "copyFromStorageLong()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 0x25, 0x3132333435363738393031323334353637383930313233343536373839303132, 0x3334353637000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 37, 22252025330403739761828227648604333229819926301751889444568374711659082559794, 23160198579300737759963791862203182809477568982966167397766552798909578608640)
    # copyToStorage()
    r = harness.call(app, "copyToStorage()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 0x25, 0x3132333435363738393031323334353637383930313233343536373839303132, 0x3334353637000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 37, 22252025330403739761828227648604333229819926301751889444568374711659082559794, 23160198579300737759963791862203182809477568982966167397766552798909578608640)
    # y() -> 0x20, 0x25, 0x3132333435363738393031323334353637383930313233343536373839303132, 0x3334353637000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "y()")
    assert tuple(r.abi_return) == (32, 37, 22252025330403739761828227648604333229819926301751889444568374711659082559794, 23160198579300737759963791862203182809477568982966167397766552798909578608640)
    # del()
    r = harness.call(app, "del()")
    # (void return — call succeeding is the assertion)
    # x() -> 0x20, 0x00
    r = harness.call(app, "x()")
    assert tuple(r.abi_return) == (32, 0)
    # invalidateXLong()
    r = harness.call(app, "invalidateXLong()")
    # (void return — call succeeding is the assertion)
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # abiEncode() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "abiEncode()", expect_revert=True)
    assert r.reverted
    # abiEncodePacked() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "abiEncodePacked()", expect_revert=True)
    assert r.reverted
    # copyToMemory() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyToMemory()", expect_revert=True)
    assert r.reverted
    # indexAccess() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "indexAccess()", expect_revert=True)
    assert r.reverted
    # arrayPushEmpty() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPushEmpty()", expect_revert=True)
    assert r.reverted
    # arrayPush() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPush()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # arrayPop() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPop()", expect_revert=True)
    assert r.reverted
    # assignToLong() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "assignToLong()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # assignTo() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "assignTo()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyFromStorageShort() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyFromStorageShort()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyFromStorageLong() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyFromStorageLong()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyToStorage() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyToStorage()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # y() -> 0x20, 0x00
    r = harness.call(app, "y()")
    assert tuple(r.abi_return) == (32, 0)
    # del() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "del()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # invalidateXShort()
    r = harness.call(app, "invalidateXShort()")
    # (void return — call succeeding is the assertion)
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # abiEncode() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "abiEncode()", expect_revert=True)
    assert r.reverted
    # abiEncodePacked() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "abiEncodePacked()", expect_revert=True)
    assert r.reverted
    # copyToMemory() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyToMemory()", expect_revert=True)
    assert r.reverted
    # indexAccess() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "indexAccess()", expect_revert=True)
    assert r.reverted
    # arrayPushEmpty() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPushEmpty()", expect_revert=True)
    assert r.reverted
    # arrayPush() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPush()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # arrayPop() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "arrayPop()", expect_revert=True)
    assert r.reverted
    # assignToLong() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "assignToLong()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # assignTo() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "assignTo()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyFromStorageShort() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyFromStorageShort()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyFromStorageLong() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyFromStorageLong()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # copyToStorage() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "copyToStorage()", expect_revert=True)
    assert r.reverted
    # x() -> FAILURE, hex"4e487b71", 0x22
    r = harness.call(app, "x()", expect_revert=True)
    assert r.reverted
    # y() -> 0x20, 0x00
    r = harness.call(app, "y()")
    assert tuple(r.abi_return) == (32, 0)

def test_long_byte_array_cleanup_after_delete(harness):
    """array/long_byte_array_cleanup_after_delete.sol"""
    app = harness.compile_and_deploy("array/long_byte_array_cleanup_after_delete.sol")
    # getArrayDataAreaSlot() -> 0x290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e563
    r = harness.call(app, "getArrayDataAreaSlot()")
    assert r.abi_return == 18569430475105882587588266137607568536673111973893317399460219858819262702947
    # getCanarySlot() -> 0x290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e566
    r = harness.call(app, "getCanarySlot()")
    assert r.abi_return == 18569430475105882587588266137607568536673111973893317399460219858819262702950
    # checkSlots() -> 0, 0, 0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0
    r = harness.call(app, "checkSlots()")
    # TODO: verify structural decoding matches expected: 0, 0, 0, 115792089237316195423570985008687907853269984665640564039457584007913129639935, 0
    assert not r.reverted
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # deleteArray()
    r = harness.call(app, "deleteArray()")
    # (void return — call succeeding is the assertion)
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935

def test_long_byte_array_cleanup_after_overwrite_with_long(harness):
    """array/long_byte_array_cleanup_after_overwrite_with_long.sol"""
    app = harness.compile_and_deploy("array/long_byte_array_cleanup_after_overwrite_with_long.sol")
    # arrayLength() ->0
    r = harness.call(app, "arrayLength()")
    assert r.abi_return == 0
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # fillArray()
    r = harness.call(app, "fillArray()")
    # (void return — call succeeding is the assertion)
    # arrayLength() ->96
    r = harness.call(app, "arrayLength()")
    assert r.abi_return == 96
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getArrayBytes(uint256,uint256): 0, 5 -> 0x20, 5, 0x0102030405000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 0, 5)
    assert tuple(r.abi_return) == (32, 5, 455867356318211655669198171616910294656723054863879369520075368192163184640)
    # getArrayBytes(uint256,uint256): 32, 5 -> 0x20, 5, 0x2122232425000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 32, 5)
    assert tuple(r.abi_return) == (32, 5, 14986639339027028362417738133592124704142064448849343707597820923851309056000)
    # getArrayBytes(uint256,uint256): 64, 5 -> 0x20, 5, 0x4142434445000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 64, 5)
    assert tuple(r.abi_return) == (32, 5, 29517411321735845069166278095567339113627405842834808045675566479510454927360)
    # shrinkArray() -> 50
    r = harness.call(app, "shrinkArray()")
    assert r.abi_return == 50
    # arrayLength() ->50
    r = harness.call(app, "arrayLength()")
    assert r.abi_return == 50
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935
    # getArrayBytes(uint256,uint256): 0, 5 -> 0x20, 5, 0x0203040506000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 0, 5)
    assert tuple(r.abi_return) == (32, 5, 909953980777862177755090045428635744953139973425925130085004916806511493120)
    # getArrayBytes(uint256,uint256): 32, 5 -> 0x20, 5, 0x2223242526000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 32, 5)
    assert tuple(r.abi_return) == (32, 5, 15440725963486678884503630007403850154438481367411389468162750472465657364480)
    # getArrayBytes(uint256,uint256): 45, 5 -> 0x20, 5, 0x2f30313233000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "getArrayBytes(uint256,uint256)", 45, 5)
    assert tuple(r.abi_return) == (32, 5, 21343852081462135671620224366956281008291901308717984355506834604452185374720)
    # getSlot1LastBytes() -> 0
    r = harness.call(app, "getSlot1LastBytes()")
    assert r.abi_return == 0
    # getDataSlotContent(uint256): 2 -> 0
    r = harness.call(app, "getDataSlotContent(uint256)", 2)
    assert r.abi_return == 0
    # canaryValue() -> 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    r = harness.call(app, "canaryValue()")
    assert r.abi_return == 115792089237316195423570985008687907853269984665640564039457584007913129639935

def test_memory(harness):
    """array/memory.sol"""
    app = harness.compile_and_deploy("array/memory.sol")
    # h(uint256[4]): 1, 2, 3, 4 -> 10
    r = harness.call(app, "h(uint256[4])", 1, 2, 3, 4)
    assert r.abi_return == 10
    # i(uint256[4]): 1, 2, 3, 4 -> 20
    r = harness.call(app, "i(uint256[4])", 1, 2, 3, 4)
    assert r.abi_return == 20

def test_memory_arrays_of_various_sizes(harness):
    """array/memory_arrays_of_various_sizes.sol"""
    app = harness.compile_and_deploy("array/memory_arrays_of_various_sizes.sol")
    # f(uint256,uint256): 3, 1 -> 1
    r = harness.call(app, "f(uint256,uint256)", 3, 1)
    assert r.abi_return == 1
    # f(uint256,uint256): 9, 5 -> 70
    r = harness.call(app, "f(uint256,uint256)", 9, 5)
    assert r.abi_return == 70

def test_nested_calldata_storage(harness):
    """array/nested_calldata_storage.sol"""
    app = harness.compile_and_deploy("array/nested_calldata_storage.sol", via_yul_behavior=True)
    # i(uint256[][2]): 0x20, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04
    r = harness.call(app, "i(uint256[][2])", 32, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    # (void return — call succeeding is the assertion)
    # tmp_i(uint256,uint256): 0, 0 -> 0x0A01
    r = harness.call(app, "tmp_i(uint256,uint256)", 0, 0)
    assert r.abi_return == 2561
    # tmp_i(uint256,uint256): 1, 0 -> 0x0B01
    r = harness.call(app, "tmp_i(uint256,uint256)", 1, 0)
    assert r.abi_return == 2817

def test_nested_calldata_storage2(harness):
    """array/nested_calldata_storage2.sol"""
    app = harness.compile_and_deploy("array/nested_calldata_storage2.sol", via_yul_behavior=True)
    # i(uint256[][]): 0x20, 2, 0x40, 0xC0, 3, 0x0A01, 0x0A02, 0x0A03, 4, 0x0B01, 0x0B02, 0x0B03, 0x0B04
    r = harness.call(app, "i(uint256[][])", 32, 2, 64, 192, 3, 2561, 2562, 2563, 4, 2817, 2818, 2819, 2820)
    # (void return — call succeeding is the assertion)
    # tmp_i(uint256,uint256): 0, 0 -> 0x0A01
    r = harness.call(app, "tmp_i(uint256,uint256)", 0, 0)
    assert r.abi_return == 2561
    # tmp_i(uint256,uint256): 1, 0 -> 0x0B01
    r = harness.call(app, "tmp_i(uint256,uint256)", 1, 0)
    assert r.abi_return == 2817

def test_reusing_memory(harness):
    """array/reusing_memory.sol"""
    app = harness.compile_and_deploy("array/reusing_memory.sol")
    # f(uint256): 0x34 -> 0x46bddb1178e94d7f2892ff5f366840eb658911794f2c3a44c450aa2c505186c1
    r = harness.call(app, "f(uint256)", 52)
    assert r.abi_return == 31997345449574252472561286867836691613551392380036115619611668045310140188353

def test_short_fixed_array_cleanup(harness):
    """array/short_fixed_array_cleanup.sol"""
    app = harness.compile_and_deploy("array/short_fixed_array_cleanup.sol")
    # fill() ->
    r = harness.call(app, "fill()")
    # (void return — call succeeding is the assertion)
    # clear() ->
    r = harness.call(app, "clear()")
    # (void return — call succeeding is the assertion)

def test_storage_array_ref(harness):
    """array/storage_array_ref.sol"""
    app = harness.compile_and_deploy("array/storage_array_ref.sol")
    # find(uint256): 7 -> -1
    r = harness.call(app, "find(uint256)", 7)
    assert r.abi_return == -1
    # add(uint256): 7 ->
    r = harness.call(app, "add(uint256)", 7)
    # (void return — call succeeding is the assertion)
    # find(uint256): 7 -> 0
    r = harness.call(app, "find(uint256)", 7)
    assert r.abi_return == 0
    # add(uint256): 11 ->
    r = harness.call(app, "add(uint256)", 11)
    # (void return — call succeeding is the assertion)
    # add(uint256): 17 ->
    r = harness.call(app, "add(uint256)", 17)
    # (void return — call succeeding is the assertion)
    # add(uint256): 27 ->
    r = harness.call(app, "add(uint256)", 27)
    # (void return — call succeeding is the assertion)
    # add(uint256): 31 ->
    r = harness.call(app, "add(uint256)", 31)
    # (void return — call succeeding is the assertion)
    # add(uint256): 32 ->
    r = harness.call(app, "add(uint256)", 32)
    # (void return — call succeeding is the assertion)
    # add(uint256): 66 ->
    r = harness.call(app, "add(uint256)", 66)
    # (void return — call succeeding is the assertion)
    # add(uint256): 177 ->
    r = harness.call(app, "add(uint256)", 177)
    # (void return — call succeeding is the assertion)
    # find(uint256): 7 -> 0
    r = harness.call(app, "find(uint256)", 7)
    assert r.abi_return == 0
    # find(uint256): 27 -> 3
    r = harness.call(app, "find(uint256)", 27)
    assert r.abi_return == 3
    # find(uint256): 32 -> 5
    r = harness.call(app, "find(uint256)", 32)
    assert r.abi_return == 5
    # find(uint256): 176 -> -1
    r = harness.call(app, "find(uint256)", 176)
    assert r.abi_return == -1
    # find(uint256): 0 -> -1
    r = harness.call(app, "find(uint256)", 0)
    assert r.abi_return == -1
    # find(uint256): 400 -> -1
    r = harness.call(app, "find(uint256)", 400)
    assert r.abi_return == -1

def test_string_allocation_bug(harness):
    """array/string_allocation_bug.sol"""
    app = harness.compile_and_deploy("array/string_allocation_bug.sol")
    # p(uint256): 0x0 -> 0xbbbb, 0xcccc, 0x80, 0xc0, 0x05, "hello", 0x05, "world"
    r = harness.call(app, "p(uint256)", 0)
    # TODO: verify expected: 0xbbbb | 0xcccc | 0x80 | 0xc0 | 0x05 | "hello" | 0x05 | "world"
    assert not r.reverted

def test_string_bytes_conversion(harness):
    """array/string_bytes_conversion.sol"""
    app = harness.compile_and_deploy("array/string_bytes_conversion.sol")
    # f(string,uint256): 0x40, 0x02, 0x06, "abcdef" -> "c"
    r = harness.call(app, "f(string,uint256)", 64, 2, 6, bytes.fromhex('616263646566'))
    # TODO: verify expected: "c"
    assert not r.reverted
    # l() -> 0x06
    r = harness.call(app, "l()")
    assert r.abi_return == 6

def test_string_literal_assign_to_storage_bytes(harness):
    """array/string_literal_assign_to_storage_bytes.sol"""
    app = harness.compile_and_deploy("array/string_literal_assign_to_storage_bytes.sol")
    # s() -> 0x20, 3, "abc"
    r = harness.call(app, "s()")
    assert r.abi_return == 'abc'
    # s1() -> 0x20, 4, "abcd"
    r = harness.call(app, "s1()")
    assert r.abi_return == 'abcd'
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # s() -> 0x20, 4, "abcd"
    r = harness.call(app, "s()")
    assert r.abi_return == 'abcd'
    # s1() -> 0x20, 3, "abc"
    r = harness.call(app, "s1()")
    assert r.abi_return == 'abc'
    # g() ->
    r = harness.call(app, "g()")
    # (void return — call succeeding is the assertion)
    # s() -> 0x20, 3, "abc"
    r = harness.call(app, "s()")
    assert r.abi_return == 'abc'
    # s1() -> 0x20, 4, "abcd"
    r = harness.call(app, "s1()")
    assert r.abi_return == 'abcd'

def test_strings_in_struct(harness):
    """array/strings_in_struct.sol"""
    app = harness.compile_and_deploy("array/strings_in_struct.sol")
    # getFirst() -> 0x0a
    r = harness.call(app, "getFirst()")
    assert r.abi_return == 10
    # getSecond() -> 0x14
    r = harness.call(app, "getSecond()")
    assert r.abi_return == 20
    # getThird() -> 0x1e
    r = harness.call(app, "getThird()")
    assert r.abi_return == 30
    # getLast() -> 0x20, 0x09, "asdfghjkl"
    r = harness.call(app, "getLast()")
    assert r.abi_return == 'asdfghjkl'
