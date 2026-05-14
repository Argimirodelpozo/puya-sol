"""Tests for the inlineAssembly category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_basefee_berlin_function(harness):
    """inlineAssembly/contracts/basefee_berlin_function.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/basefee_berlin_function.sol", evm_version='berlin')
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 1000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1000

def test_blobbasefee_shanghai_function(harness):
    """inlineAssembly/contracts/blobbasefee_shanghai_function.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/blobbasefee_shanghai_function.sol", evm_version='shanghai')
    # f() -> 999
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 999
    # g() -> 1000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1000

def test_blobhash(harness):
    """inlineAssembly/contracts/blobhash.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/blobhash.sol")
    # f() -> 0x0100000000000000000000000000000000000000000000000000000000000001
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 452312848583266388373324160190187140051835877600158453279131187530910662657

def test_blobhash_index_exceeding_blob_count(harness):
    """inlineAssembly/contracts/blobhash_index_exceeding_blob_count.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/blobhash_index_exceeding_blob_count.sol")
    # f() -> 0x00
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_blobhash_pre_cancun(harness):
    """inlineAssembly/contracts/blobhash_pre_cancun.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/blobhash_pre_cancun.sol", evm_version='shanghai')
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 1000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1000

def test_calldata_array_assign_dynamic(harness):
    """inlineAssembly/contracts/calldata_array_assign_dynamic.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_array_assign_dynamic.sol")
    # f(uint256[2][]): 0x0, 1, 8, 7, 6, 5 -> 0x20, 2, 8, 7, 6, 5
    r = harness.call(app, "f(uint256[2][])", 0, 1, 8, 7, 6, 5)
    # TODO: verify structural decoding matches expected: 32, 2, 8, 7, 6, 5
    assert not r.reverted

def test_calldata_array_assign_static(harness):
    """inlineAssembly/contracts/calldata_array_assign_static.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_array_assign_static.sol")
    # f(uint256[2][2]): 0x0, 8, 7, 6, 5 -> 8, 7, 6, 5
    r = harness.call(app, "f(uint256[2][2])", 0, 8, 7, 6, 5)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 7, 6, 5)

def test_calldata_array_read(harness):
    """inlineAssembly/contracts/calldata_array_read.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_array_read.sol")
    # f(uint256[2][]): 0x20, 2, 1, 2, 3, 4 -> 0x44, 2, 0x84
    r = harness.call(app, "f(uint256[2][])", 32, 2, 1, 2, 3, 4)
    assert tuple(as_int(x) for x in r.abi_return) == (68, 2, 132)

def test_calldata_assign(harness):
    """inlineAssembly/contracts/calldata_assign.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_assign.sol")
    # f(bytes): 0x20, 0, 0 -> 0x20, 3, 0x5754f80000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f(bytes)", 32, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 3, 39501344613279564131983482746062531042151437873576435683632308542105309937664)

def test_calldata_assign_from_nowhere(harness):
    """inlineAssembly/contracts/calldata_assign_from_nowhere.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_assign_from_nowhere.sol")
    # f() -> 0x20, 4, 0x26121ff000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 4, 17219911917854084299749778639755835327755045716242581057573779540915269926912)

def test_calldata_length_read(harness):
    """inlineAssembly/contracts/calldata_length_read.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_length_read.sol")
    # lenBytesRead(bytes): 0x20, 4, "abcd" -> 4
    r = harness.call(app, "lenBytesRead(bytes)", 'abcd')
    assert as_int(r.abi_return) == 4
    # lenBytesRead(bytes): 0x20, 0, "abcd" -> 0x00
    r = harness.call(app, "lenBytesRead(bytes)", '')
    assert as_int(r.abi_return) == 0
    # lenBytesRead(bytes): 0x20, 0x21, "abcd", "ef" -> 33
    r = harness.call(app, "lenBytesRead(bytes)", 32, 33, bytes.fromhex('61626364'), bytes.fromhex('6566'))
    assert as_int(r.abi_return) == 33
    # lenStringRead(string): 0x20, 4, "abcd" -> 4
    r = harness.call(app, "lenStringRead(string)", 'abcd')
    assert as_int(r.abi_return) == 4
    # lenStringRead(string): 0x20, 0, "abcd" -> 0x00
    r = harness.call(app, "lenStringRead(string)", '')
    assert as_int(r.abi_return) == 0
    # lenStringRead(string): 0x20, 0x21, "abcd", "ef" -> 33
    r = harness.call(app, "lenStringRead(string)", 32, 33, bytes.fromhex('61626364'), bytes.fromhex('6566'))
    assert as_int(r.abi_return) == 33

def test_calldata_offset_read(harness):
    """inlineAssembly/contracts/calldata_offset_read.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_offset_read.sol")
    # f(bytes): 0x20, 0, 0 -> 0x44
    r = harness.call(app, "f(bytes)", 32, 0, 0)
    assert as_int(r.abi_return) == 68
    # f(bytes): 0x22, 0, 0, 0 -> 0x46
    r = harness.call(app, "f(bytes)", 34, 0, 0, 0)
    assert as_int(r.abi_return) == 70
    # f(uint256,bytes,uint256): 7, 0x60, 8, 2, 0 -> 0x84, 2
    r = harness.call(app, "f(uint256,bytes,uint256)", 7, 96, 8, 2, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (132, 2)
    # f(uint256,bytes,uint256): 0, 0, 0 -> 0x24, 0x00
    r = harness.call(app, "f(uint256,bytes,uint256)", 0, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (36, 0)

def test_calldata_offset_read_write(harness):
    """inlineAssembly/contracts/calldata_offset_read_write.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_offset_read_write.sol")
    # f(uint256,bytes,uint256): 7, 0x60, 8, 2, 0 -> 8, 0x14
    r = harness.call(app, "f(uint256,bytes,uint256)", 7, 96, 8, 2, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 20)
    # f(uint256,bytes,uint256): 0, 0, 0 -> 8, 0x14
    r = harness.call(app, "f(uint256,bytes,uint256)", 0, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 20)

def test_calldata_struct_assign(harness):
    """inlineAssembly/contracts/calldata_struct_assign.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_struct_assign.sol")
    # f((uint256),(uint256,uint256)): 0x42, 0x07, 0x77 -> 0x07, 0x42
    r = harness.call(app, "f((uint256),(uint256,uint256))", 66, 7, 119)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 66)

def test_calldata_struct_assign_and_return(harness):
    """inlineAssembly/contracts/calldata_struct_assign_and_return.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_struct_assign_and_return.sol")
    # g(): 0xCAFFEE, 0x42, 0x21 -> 0x42, 0x21
    r = harness.call(app, "g()", 13303790, 66, 33)
    assert tuple(as_int(x) for x in r.abi_return) == (66, 33)
    # g(): 0xCAFFEE, 0x4242, 0x2121 -> FAILURE
    r = harness.call(app, "g()", 13303790, 16962, 8481, expect_revert=True)
    assert r.reverted
    # g(): 0xCAFFEE, 0x42 -> 0x42, 0
    r = harness.call(app, "g()", 13303790, 66)
    assert tuple(as_int(x) for x in r.abi_return) == (66, 0)
    # h() -> 0x42
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 66
    # i() -> FAILURE
    r = harness.call(app, "i()", expect_revert=True)
    assert r.reverted

def test_chainid(harness):
    """inlineAssembly/contracts/chainid.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/chainid.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_clz(harness):
    """inlineAssembly/contracts/clz.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/clz.sol", evm_version='osaka')
    # f() -> 256
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 256
    # g() -> 255
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 255
    # h() -> 1
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 1

def test_clz_pre_osaka(harness):
    """inlineAssembly/contracts/clz_pre_osaka.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/clz_pre_osaka.sol", evm_version='prague')
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 1000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1000

def test_constant_access(harness):
    """inlineAssembly/contracts/constant_access.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/constant_access.sol")
    # f() -> 2, left(0xabcd), left(0x616263), true, 0x1212121212121212121212121212121212121212
    r = harness.call(app, "f()")
    # TODO: verify expected: 2 | left(0xabcd) | left(0x616263) | true | 0x1212121212121212121212121212121212121212
    assert not r.reverted

def test_constant_access_referencing(harness):
    """inlineAssembly/contracts/constant_access_referencing.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/constant_access_referencing.sol")
    # f() -> 2, left(0xabcd), left(0x616263), true, 0x1212121212121212121212121212121212121212
    r = harness.call(app, "f()")
    # TODO: verify expected: 2 | left(0xabcd) | left(0x616263) | true | 0x1212121212121212121212121212121212121212
    assert not r.reverted

def test_difficulty(harness):
    """inlineAssembly/contracts/difficulty.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/difficulty.sol", evm_version='london')
    # f() -> 200000000
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 200000000

def test_external_function_pointer_address(harness):
    """inlineAssembly/contracts/external_function_pointer_address.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/external_function_pointer_address.sol")
    # testYul() -> 0x1234
    r = harness.call(app, "testYul()")
    assert as_int(r.abi_return) == 4660
    # testSol() -> 0x1234
    r = harness.call(app, "testSol()")
    assert as_int(r.abi_return) == 4660

def test_external_function_pointer_address_assignment(harness):
    """inlineAssembly/contracts/external_function_pointer_address_assignment.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/external_function_pointer_address_assignment.sol")
    # testYul(address): 0x1234567890 -> 0x1234567890
    r = harness.call(app, "testYul(address)", encoding.encode_address((78187493520).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 78187493520
    # testYul(address): 0xC0FFEE3EA7 -> 0xC0FFEE3EA7
    r = harness.call(app, "testYul(address)", encoding.encode_address((828927524519).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 828927524519

def test_external_function_pointer_selector(harness):
    """inlineAssembly/contracts/external_function_pointer_selector.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/external_function_pointer_selector.sol")
    # testYul() -> 0x89aac53b
    r = harness.call(app, "testYul()")
    assert as_int(r.abi_return) == 2309670203
    # testSol() -> 0xe16b4a9b
    r = harness.call(app, "testSol()")
    assert as_int(r.abi_return) == 3781905051

def test_external_function_pointer_selector_assignment(harness):
    """inlineAssembly/contracts/external_function_pointer_selector_assignment.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/external_function_pointer_selector_assignment.sol")
    # testYul(uint32): 0x12345678 -> 0x12345678
    r = harness.call(app, "testYul(uint32)", 305419896)
    assert as_int(r.abi_return) == 305419896
    # testYul(uint32): 0xABCDEF00 -> 0xABCDEF00
    r = harness.call(app, "testYul(uint32)", 2882400000)
    assert as_int(r.abi_return) == 2882400000

def test_external_identifier_access_shadowing(harness):
    """inlineAssembly/contracts/external_identifier_access_shadowing.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/external_identifier_access_shadowing.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_for_loop_break(harness):
    """inlineAssembly/contracts/for_loop_break.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/for_loop_break.sol")
    # f() -> 6
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 6

def test_for_loop_continue(harness):
    """inlineAssembly/contracts/for_loop_continue.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/for_loop_continue.sol")
    # f() -> 5
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 5

def test_for_loop_nested(harness):
    """inlineAssembly/contracts/for_loop_nested.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/for_loop_nested.sol")
    # f(uint256): 0 -> 2
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 2
    # f(uint256): 1 -> 18
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 18
    # f(uint256): 2 -> 10
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 10
    # f(uint256): 4 -> 91
    r = harness.call(app, "f(uint256)", 4)
    assert as_int(r.abi_return) == 91

def test_function_name_clash(harness):
    """inlineAssembly/contracts/function_name_clash.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/function_name_clash.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 2
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 2

def test_inline_assembly_embedded_function_call(harness):
    """inlineAssembly/contracts/inline_assembly_embedded_function_call.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_embedded_function_call.sol")
    # f() -> 0x1, 0x4, 0x7, 0x10
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 4, 7, 16)

def test_inline_assembly_for(harness):
    """inlineAssembly/contracts/inline_assembly_for.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_for.sol")
    # f(uint256): 0 -> 1
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 1
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # f(uint256): 3 -> 6
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 6
    # f(uint256): 4 -> 24
    r = harness.call(app, "f(uint256)", 4)
    assert as_int(r.abi_return) == 24

def test_inline_assembly_for2(harness):
    """inlineAssembly/contracts/inline_assembly_for2.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_for2.sol")
    # f(uint256): 0 -> 0, 2, 0
    r = harness.call(app, "f(uint256)", 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2, 0)
    # f(uint256): 1 -> 1, 4, 3
    r = harness.call(app, "f(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 4, 3)
    # f(uint256): 2 -> 0, 2, 0
    r = harness.call(app, "f(uint256)", 2)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2, 0)

def test_inline_assembly_function_call(harness):
    """inlineAssembly/contracts/inline_assembly_function_call.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call.sol")
    # f() -> 1, 2, 7
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 7)

def test_inline_assembly_function_call2(harness):
    """inlineAssembly/contracts/inline_assembly_function_call2.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call2.sol")
    # f() -> 0x1, 0x2, 0x7, 0x10
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 7, 16)

def test_inline_assembly_function_call_assignment(harness):
    """inlineAssembly/contracts/inline_assembly_function_call_assignment.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call_assignment.sol")
    # f() -> 1, 2, 7
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 7)

def test_inline_assembly_if(harness):
    """inlineAssembly/contracts/inline_assembly_if.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_if.sol")
    # f(uint256): 0 -> 0
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # f(uint256): 1 -> 0
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 0
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # f(uint256): 3 -> 2
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 2

def test_inline_assembly_in_modifiers(harness):
    """inlineAssembly/contracts/inline_assembly_in_modifiers.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_in_modifiers.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # g() -> FAILURE
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted

def test_inline_assembly_memory_access(harness):
    """inlineAssembly/contracts/inline_assembly_memory_access.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_memory_access.sol")
    # test() -> 0x20, 0x5, "12345"
    r = harness.call(app, "test()")
    assert r.abi_return == '12345'

def test_inline_assembly_read_and_write_stack(harness):
    """inlineAssembly/contracts/inline_assembly_read_and_write_stack.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_read_and_write_stack.sol")
    # f() -> 45
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 45

def test_inline_assembly_recursion(harness):
    """inlineAssembly/contracts/inline_assembly_recursion.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_recursion.sol")
    # f(uint256): 0 -> 1
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 1
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # f(uint256): 3 -> 6
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 6
    # f(uint256): 4 -> 24
    r = harness.call(app, "f(uint256)", 4)
    assert as_int(r.abi_return) == 24

def test_inline_assembly_storage_access(harness):
    """inlineAssembly/contracts/inline_assembly_storage_access.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_storage_access.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # z() -> 7
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 7

def test_inline_assembly_storage_access_inside_function(harness):
    """inlineAssembly/contracts/inline_assembly_storage_access_inside_function.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_storage_access_inside_function.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # z() -> 7
    r = harness.call(app, "z()")
    assert as_int(r.abi_return) == 7

def test_inline_assembly_storage_access_local_var(harness):
    """inlineAssembly/contracts/inline_assembly_storage_access_local_var.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_storage_access_local_var.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7

def test_inline_assembly_storage_access_via_pointer(harness):
    """inlineAssembly/contracts/inline_assembly_storage_access_via_pointer.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_storage_access_via_pointer.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # a() -> 7
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 7
    # separator() -> 0
    r = harness.call(app, "separator()")
    assert as_int(r.abi_return) == 0
    # separator2() -> 0
    r = harness.call(app, "separator2()")
    assert as_int(r.abi_return) == 0

def test_inline_assembly_switch(harness):
    """inlineAssembly/contracts/inline_assembly_switch.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_switch.sol")
    # f(uint256): 0 -> 2
    r = harness.call(app, "f(uint256)", 0)
    assert as_int(r.abi_return) == 2
    # f(uint256): 1 -> 8
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 8
    # f(uint256): 2 -> 9
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 9
    # f(uint256): 3 -> 2
    r = harness.call(app, "f(uint256)", 3)
    assert as_int(r.abi_return) == 2

def test_inline_assembly_transient_storage_access_inside_function(harness):
    """inlineAssembly/contracts/inline_assembly_transient_storage_access_inside_function.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_transient_storage_access_inside_function.sol")
    # f() -> 7
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 7

def test_inline_assembly_write_to_stack(harness):
    """inlineAssembly/contracts/inline_assembly_write_to_stack.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_write_to_stack.sol")
    # f() -> 7, "abcdef"
    r = harness.call(app, "f()")
    # TODO: verify expected: 7 | "abcdef"
    assert not r.reverted

def test_inlineasm_empty_let(harness):
    """inlineAssembly/contracts/inlineasm_empty_let.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inlineasm_empty_let.sol")
    # f() -> 0, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)

def test_keccak256_assembly(harness):
    """inlineAssembly/contracts/keccak256_assembly.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_assembly.sol")
    # f() -> 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 89477152217924674838424037953991966239322087453347756267410168184682657981552

def test_keccak256_optimization(harness):
    """inlineAssembly/contracts/keccak256_optimization.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_optimization.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

def test_keccak256_optimizer_bug_different_memory_location(harness):
    """inlineAssembly/contracts/keccak256_optimizer_bug_different_memory_location.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_optimizer_bug_different_memory_location.sol")
    # f() -> false
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is False

def test_keccak256_optimizer_cache_bug(harness):
    """inlineAssembly/contracts/keccak256_optimizer_cache_bug.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_optimizer_cache_bug.sol")
    # val() -> true
    r = harness.call(app, "val()")
    assert bool(as_int(r.abi_return)) is True

def test_keccak_optimization_bug_string(harness):
    """inlineAssembly/contracts/keccak_optimization_bug_string.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak_optimization_bug_string.sol")
    # f(string): "" -> false
    r = harness.call(app, "f(string)", bytes.fromhex(''))
    assert bool(as_int(r.abi_return)) is False
    # f(string): 0x20, 5, "hello" -> false
    r = harness.call(app, "f(string)", 'hello')
    assert bool(as_int(r.abi_return)) is False
    # f(string): 0x20, 0x2e, 29457663690442756349866640336617293820574110049925353194191585327958485180523, 45859201465615193776739262511799714667061496775486067316261261194408342061056 -> false
    r = harness.call(app, "f(string)", 32, 46, 0x4120726174686572206c6f6e6720737472696e672e20536f6c6964697479206b, 0x656363616b32353620746573742e000000000000000000000000000000000000)
    assert bool(as_int(r.abi_return)) is False

def test_keccak_yul_optimization(harness):
    """inlineAssembly/contracts/keccak_yul_optimization.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak_yul_optimization.sol")
    # f() -> 0xcdb56c384a9682c600315e3470157a4cf7638d0d33e9dae5c40ffd2644fc5a80
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 93044680184511990070700303972941433433889251500196647875449442509618203613824
    # g() -> 0xcdb56c384a9682c600315e3470157a4cf7638d0d33e9dae5c40ffd2644fc5a80
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 93044680184511990070700303972941433433889251500196647875449442509618203613824

def test_leave(harness):
    """inlineAssembly/contracts/leave.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/leave.sol")
    # f() -> 2
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 2

def test_mcopy(harness):
    """inlineAssembly/contracts/mcopy.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/mcopy.sol")
    # f(bytes): 0x20, 0x20, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff -> 0x20, 0x20, 0x0000000000000000776655443322110000112233445566770000000000000000
    r = harness.call(app, "f(bytes)", 32, 32, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 2927673070831330322292941411582002110169683550948852498432)

def test_mcopy_as_identifier_pre_cancun(harness):
    """inlineAssembly/contracts/mcopy_as_identifier_pre_cancun.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/mcopy_as_identifier_pre_cancun.sol", evm_version='shanghai')
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 1000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 1000

def test_mcopy_empty(harness):
    """inlineAssembly/contracts/mcopy_empty.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/mcopy_empty.sol")
    # mcopy_zero(bytes): 0x20, 0x20, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff -> 0x20, 0x20, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff
    r = harness.call(app, "mcopy_zero(bytes)", 32, 32, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 115761816795685524522806652725025505785880228478355084463266898959573796646655)

def test_mcopy_overlap(harness):
    """inlineAssembly/contracts/mcopy_overlap.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/mcopy_overlap.sol")
    # mcopy_to_right_overlap()    -> 0x20, 0x60, 0x2222222222222222333333333333333344444444444444445555555555555555, 0x4444444444444444555555555555555566666666666666667777777777777777, 0x88888888888888889999999999999999ccccccccccccccccdddddddddddddddd
    r = harness.call(app, "mcopy_to_right_overlap()")
    # TODO: verify structural decoding matches expected: 32, 96, 15438945231642159390227938116850833098047736074760648863899956320786384770389, 30877890463284318780037402784675887478483734030179390735827634188508135389047, 61755780926568637559656332120325996239401100923272999608144773204942539054557
    assert not r.reverted
    # mcopy_to_left_overlap()     -> 0x20, 0x60, 0x2222222222222222333333333333333366666666666666667777777777777777, 0x88888888888888889999999999999999aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb, 0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd
    r = harness.call(app, "mcopy_to_left_overlap()")
    # TODO: verify structural decoding matches expected: 32, 96, 15438945231642159390227938116850833098093107057016773992361739601777287198583, 61755780926568637559656332120325996239355729941016874479682989923951636626363, 77194726158210796949465796788151050619791727896435616351610667791673387245021
    assert not r.reverted
    # mcopy_in_place()            -> 0x20, 0x60, 0x2222222222222222333333333333333344444444444444445555555555555555, 0x6666666666666666777777777777777788888888888888889999999999999999, 0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd
    r = harness.call(app, "mcopy_in_place()")
    # TODO: verify structural decoding matches expected: 32, 96, 15438945231642159390227938116850833098047736074760648863899956320786384770389, 46316835694926478169846867452500941858919731985598132607755312056229886007705, 77194726158210796949465796788151050619791727896435616351610667791673387245021
    assert not r.reverted
    # mcopy_to_right_no_overlap() -> 0x20, 0x60, 0x2222222222222222333333333333333344444444444444445555555555555555, 0x6666666666666666777777777777777744444444444444445555555555555555, 0x66666666666666667777777777777777ccccccccccccccccdddddddddddddddd
    r = harness.call(app, "mcopy_to_right_no_overlap()")
    # TODO: verify structural decoding matches expected: 32, 96, 15438945231642159390227938116850833098047736074760648863899956320786384770389, 46316835694926478169846867452500941858828990021085882350831745494248081151317, 46316835694926478169846867452500941859010473950110382864678878618211690864093
    assert not r.reverted
    # mcopy_to_left_no_overlap()  -> 0x20, 0x60, 0x2222222222222222333333333333333388888888888888889999999999999999, 0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb88888888888888889999999999999999, 0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd
    r = harness.call(app, "mcopy_to_left_no_overlap()")
    # TODO: verify structural decoding matches expected: 32, 96, 15438945231642159390227938116850833098138478039272899120823522882768189626777, 77194726158210796949465796788151050619700985931923366094687101229691582388633, 77194726158210796949465796788151050619791727896435616351610667791673387245021
    assert not r.reverted

def test_optimize_memory_store_multi_block(harness):
    """inlineAssembly/contracts/optimize_memory_store_multi_block.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/optimize_memory_store_multi_block.sol")
    # f() -> 0x42
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 66
    # g() -> true
    r = harness.call(app, "g()")
    assert bool(as_int(r.abi_return)) is True

def test_optimize_memory_store_multi_block_bugreport(harness):
    """inlineAssembly/contracts/optimize_memory_store_multi_block_bugreport.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/optimize_memory_store_multi_block_bugreport.sol")
    # test() -> 10
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 10

def test_prevrandao(harness):
    """inlineAssembly/contracts/prevrandao.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/prevrandao.sol")
    # f() -> 0xa86c2e601b6c44eb4848f7d23d9df3113fbcac42041c49cbed5000cb4f118777
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 76179698116359622413486155173975521935699888105599510728246182663625645328247

def test_selfbalance(harness):
    """inlineAssembly/contracts/selfbalance.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/selfbalance.sol")
    # f(), 254 wei -> 254
    r = harness.call(app, "f()", payment_wei=254)
    assert as_int(r.abi_return) == 254

def test_shadowing_local_function_opcode(harness):
    """inlineAssembly/contracts/shadowing_local_function_opcode.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/shadowing_local_function_opcode.sol")
    # g() -> 7, 3
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 3)

def test_slot_access(harness):
    """inlineAssembly/contracts/slot_access.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/slot_access.sol")
    # get() -> 0
    r = harness.call(app, "get()")
    assert as_int(r.abi_return) == 0
    # mappingAccess(uint256): 1 -> 0, 0
    r = harness.call(app, "mappingAccess(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # set(uint256): 4
    r = harness.call(app, "set(uint256)", 4)
    # (void return — call succeeding is the assertion)
    # get() -> 4
    r = harness.call(app, "get()")
    assert as_int(r.abi_return) == 4
    # mappingAccess(uint256): 1 -> 4, 0
    r = harness.call(app, "mappingAccess(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (4, 0)

def test_slot_access_via_mapping_pointer(harness):
    """inlineAssembly/contracts/slot_access_via_mapping_pointer.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/slot_access_via_mapping_pointer.sol")
    # f(uint256): 0 -> 0, 0
    r = harness.call(app, "f(uint256)", 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # f(uint256): 1 -> 1, 0
    r = harness.call(app, "f(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)
    # f(uint256): 2 -> 2, 0
    r = harness.call(app, "f(uint256)", 2)
    assert tuple(as_int(x) for x in r.abi_return) == (2, 0)

def test_tload_tstore_not_reserved_before_cancun(harness):
    """inlineAssembly/contracts/tload_tstore_not_reserved_before_cancun.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/tload_tstore_not_reserved_before_cancun.sol", evm_version='shanghai')
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1
    # g() -> 5
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 5

def test_transient_storage_creation(harness):
    """inlineAssembly/contracts/transient_storage_creation.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_creation.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_transient_storage_low_level_calls(harness):
    """inlineAssembly/contracts/transient_storage_low_level_calls.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_low_level_calls.sol")
    # testDelegateCall() -> true
    r = harness.call(app, "testDelegateCall()")
    assert bool(as_int(r.abi_return)) is True
    # testCall() -> true
    r = harness.call(app, "testCall()")
    assert bool(as_int(r.abi_return)) is True
    # tloadAllowedStaticCall() -> true
    r = harness.call(app, "tloadAllowedStaticCall()")
    assert bool(as_int(r.abi_return)) is True
    # tstoreNotAllowedStaticCall() -> true
    r = harness.call(app, "tstoreNotAllowedStaticCall()")
    assert bool(as_int(r.abi_return)) is True

def test_transient_storage_multiple_calls_different_transactions(harness):
    """inlineAssembly/contracts/transient_storage_multiple_calls_different_transactions.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_multiple_calls_different_transactions.sol")
    # test() ->
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)
    # test() ->
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)

def test_transient_storage_multiple_transactions(harness):
    """inlineAssembly/contracts/transient_storage_multiple_transactions.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_multiple_transactions.sol")
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0
    # h() -> 0x63
    r = harness.call(app, "h()")
    assert as_int(r.abi_return) == 99
    # g() -> 0
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 0

def test_transient_storage_reset_between_creation_runtime(harness):
    """inlineAssembly/contracts/transient_storage_reset_between_creation_runtime.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_reset_between_creation_runtime.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0

def test_transient_storage_sanity_checks(harness):
    """inlineAssembly/contracts/transient_storage_sanity_checks.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_sanity_checks.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
    # g() -> 0x2a, 0, 0
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (42, 0, 0)

def test_transient_storage_selfdestruct(harness):
    """inlineAssembly/contracts/transient_storage_selfdestruct.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_selfdestruct.sol")
    # destroy() ->
    r = harness.call(app, "destroy()")
    # (void return — call succeeding is the assertion)
    # createAndDestroy() ->
    r = harness.call(app, "createAndDestroy()")
    # (void return — call succeeding is the assertion)

def test_transient_storage_simple_reentrancy_lock(harness):
    """inlineAssembly/contracts/transient_storage_simple_reentrancy_lock.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/transient_storage_simple_reentrancy_lock.sol")
    # f(bool): false ->
    r = harness.call(app, "f(bool)", False)
    # (void return — call succeeding is the assertion)
    # f(bool): true -> FAILURE
    r = harness.call(app, "f(bool)", True, expect_revert=True)
    assert r.reverted

def test_truefalse(harness):
    """inlineAssembly/contracts/truefalse.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/truefalse.sol")
    # f() -> 1, 0
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)

def test_tstore_hidden_staticcall(harness):
    """inlineAssembly/contracts/tstore_hidden_staticcall.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/tstore_hidden_staticcall.sol")
    # test() -> FAILURE
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted
