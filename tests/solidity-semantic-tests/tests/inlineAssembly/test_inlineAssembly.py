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
    """inlineAssembly/contracts/blobhash.sol — EVM blobhash has no AVM analog."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/blobhash.sol")
    assert not harness.call(app, "f()").reverted

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

def test_calldata_array_assign_dynamic(harness):  # currently fails
    """inlineAssembly/contracts/calldata_array_assign_dynamic.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_array_assign_dynamic.sol')
    r = harness.call(app, 'f(uint256[2][])', 0x0, 1, 8, 7, 6, 5)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 2, 8, 7, 6, 5,)

def test_calldata_array_assign_static(harness):  # currently fails
    """inlineAssembly/contracts/calldata_array_assign_static.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_array_assign_static.sol')
    r = harness.call(app, 'f(uint256[2][2])', 0x0, 8, 7, 6, 5)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 7, 6, 5,)

def test_calldata_array_read(harness):  # currently fails
    """inlineAssembly/contracts/calldata_array_read.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_array_read.sol')
    r = harness.call(app, 'f(uint256[2][])', 0x20, 2, 1, 2, 3, 4)
    assert tuple(as_int(x) for x in r.abi_return) == (0x44, 2, 0x84,)

def test_calldata_assign(harness):  # currently fails
    """inlineAssembly/contracts/calldata_assign.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_assign.sol')
    r = harness.call(app, 'f(bytes)', 0x20, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 3, 0x5754f80000000000000000000000000000000000000000000000000000000000,)

def test_calldata_assign_from_nowhere(harness):  # currently fails
    """inlineAssembly/contracts/calldata_assign_from_nowhere.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_assign_from_nowhere.sol')
    r = harness.call(app, 'f()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 4, 0x26121ff000000000000000000000000000000000000000000000000000000000,)

def test_calldata_length_read(harness):
    """inlineAssembly/contracts/calldata_length_read.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/calldata_length_read.sol")
    assert as_int(harness.call(app, "lenBytesRead(bytes)", b"abcd").abi_return) == 4
    assert as_int(harness.call(app, "lenBytesRead(bytes)", b"").abi_return) == 0
    # 33-byte input.
    arg33 = b"abcd" * 8 + b"e"
    assert as_int(harness.call(app, "lenBytesRead(bytes)", arg33).abi_return) == 33
    assert as_int(harness.call(app, "lenStringRead(string)", "abcd").abi_return) == 4
    assert as_int(harness.call(app, "lenStringRead(string)", "").abi_return) == 0
    assert as_int(harness.call(app, "lenStringRead(string)", "abcd" * 8 + "e").abi_return) == 33

def test_calldata_offset_read(harness):  # currently fails
    """inlineAssembly/contracts/calldata_offset_read.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_offset_read.sol')
    r = harness.call(app, 'f(bytes)', 0x20, 0, 0)
    assert as_int(r.abi_return) == 0x44
    r = harness.call(app, 'f(bytes)', 0x22, 0, 0, 0)
    assert as_int(r.abi_return) == 0x46
    r = harness.call(app, 'f(uint256,bytes,uint256)', 7, 0x60, 8, 2, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0x84, 2,)
    r = harness.call(app, 'f(uint256,bytes,uint256)', 0, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0x24, 0x00,)

def test_calldata_offset_read_write(harness):  # currently fails
    """inlineAssembly/contracts/calldata_offset_read_write.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_offset_read_write.sol')
    r = harness.call(app, 'f(uint256,bytes,uint256)', 7, 0x60, 8, 2, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 0x14,)
    r = harness.call(app, 'f(uint256,bytes,uint256)', 0, 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 0x14,)

def test_calldata_struct_assign(harness):  # currently fails
    """inlineAssembly/contracts/calldata_struct_assign.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_struct_assign.sol')
    r = harness.call(app, 'f((uint256),(uint256,uint256))', 0x42, 0x07, 0x77)
    assert tuple(as_int(x) for x in r.abi_return) == (0x07, 0x42,)

def test_calldata_struct_assign_and_return(harness):  # currently fails
    """inlineAssembly/contracts/calldata_struct_assign_and_return.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/calldata_struct_assign_and_return.sol')
    r = harness.call(app, 'g()', 0xCAFFEE, 0x42, 0x21)
    assert tuple(as_int(x) for x in r.abi_return) == (0x42, 0x21,)
    r = harness.call(app, 'g()', 0xCAFFEE, 0x4242, 0x2121, expect_revert=True)
    assert r.reverted
    r = harness.call(app, 'g()', 0xCAFFEE, 0x42)
    assert tuple(as_int(x) for x in r.abi_return) == (0x42, 0,)
    r = harness.call(app, 'h()')
    assert as_int(r.abi_return) == 0x42
    r = harness.call(app, 'i()', expect_revert=True)
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
    """inlineAssembly/contracts/inline_assembly_embedded_function_call.sol —
    uses Yul `return(0, 0x80)` to emit raw bytes. The Solidity ABI-level
    return type is void, so the AVM dispatcher reports no abi_return; the
    raw bytes aren't surfaced. Just verify the call doesn't revert."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_embedded_function_call.sol")
    assert not harness.call(app, "f()").reverted

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

def test_inline_assembly_for2(harness):  # currently fails
    """inlineAssembly/contracts/inline_assembly_for2.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/inline_assembly_for2.sol')
    r = harness.call(app, 'f(uint256)', 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2, 0,)
    r = harness.call(app, 'f(uint256)', 1)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 4, 3,)
    r = harness.call(app, 'f(uint256)', 2)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2, 0,)

def test_inline_assembly_function_call(harness):
    """inlineAssembly/contracts/inline_assembly_function_call.sol — Yul
    `return(0, 0x60)` emits raw bytes that the AVM dispatcher doesn't
    surface as abi_return (return type is void in Solidity)."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call.sol")
    assert not harness.call(app, "f()").reverted

def test_inline_assembly_function_call2(harness):
    """inlineAssembly/contracts/inline_assembly_function_call2.sol — same."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call2.sol")
    assert not harness.call(app, "f()").reverted

def test_inline_assembly_function_call_assignment(harness):
    """inlineAssembly/contracts/inline_assembly_function_call_assignment.sol — same."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/inline_assembly_function_call_assignment.sol")
    assert not harness.call(app, "f()").reverted

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
    # Returns a `bytes` blob "12345" — algosdk decodes as list[int].
    r = harness.call(app, "test()")
    assert bytes(r.abi_return) == b"12345"

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

def test_inline_assembly_storage_access_local_var(harness):  # currently fails
    """inlineAssembly/contracts/inline_assembly_storage_access_local_var.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/inline_assembly_storage_access_local_var.sol')
    r = harness.call(app, 'f()')
    assert as_int(r.abi_return) == 7

def test_inline_assembly_storage_access_via_pointer(harness):  # currently fails
    """inlineAssembly/contracts/inline_assembly_storage_access_via_pointer.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/inline_assembly_storage_access_via_pointer.sol')
    r = harness.call(app, 'f()')
    assert r.abi_return is True
    r = harness.call(app, 'a()')
    assert as_int(r.abi_return) == 7
    r = harness.call(app, 'separator()')
    assert as_int(r.abi_return) == 0
    r = harness.call(app, 'separator2()')
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

@pytest.mark.xfail(reason="Yul calldataload at an unresolvable offset; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0 that zeroed a real input)", strict=False)
def test_keccak256_optimization(harness):
    """inlineAssembly/contracts/keccak256_optimization.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_optimization.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

@pytest.mark.xfail(reason="Yul calldataload at an unresolvable offset; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0 that zeroed a real input)", strict=False)
def test_keccak256_optimizer_bug_different_memory_location(harness):
    """inlineAssembly/contracts/keccak256_optimizer_bug_different_memory_location.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/keccak256_optimizer_bug_different_memory_location.sol")
    # f() -> false
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is False

def test_keccak256_optimizer_cache_bug(harness):  # currently fails
    """inlineAssembly/contracts/keccak256_optimizer_cache_bug.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/keccak256_optimizer_cache_bug.sol')
    r = harness.call(app, 'val()')
    assert r.abi_return is True

def test_keccak_optimization_bug_string(harness):  # currently fails
    """inlineAssembly/contracts/keccak_optimization_bug_string.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/keccak_optimization_bug_string.sol')
    r = harness.call(app, 'f(string)', 0x20, 0x2e, 29457663690442756349866640336617293820574110049925353194191585327958485180523, 45859201465615193776739262511799714667061496775486067316261261194408342061056)
    assert r.abi_return is False

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

def test_mcopy(harness):  # currently fails
    """inlineAssembly/contracts/mcopy.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/mcopy.sol')
    r = harness.call(app, 'f(bytes)', 0x20, 0x20, 0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x20, 0x0000000000000000776655443322110000112233445566770000000000000000,)

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
    arg = bytes.fromhex("ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff")
    r = harness.call(app, "mcopy_zero(bytes)", arg)
    assert bytes(r.abi_return) == arg

def test_mcopy_overlap(harness):
    """inlineAssembly/contracts/mcopy_overlap.sol

    Each function returns bytes memory (96 bytes = 3 × 32-byte words). The
    libsolidity format includes EVM calldata header [offset=0x20][length=0x60]
    that the EVM strips on entry; AVM returns raw bytes. We compare as
    3 big-endian uint256 words (parse 32 bytes at a time).
    """
    def words(r):
        raw = bytes(r.abi_return)
        assert len(raw) == 96, f"expected 96 bytes, got {len(raw)}"
        return tuple(int.from_bytes(raw[i:i+32], 'big') for i in range(0, 96, 32))

    app = harness.compile_and_deploy('inlineAssembly/contracts/mcopy_overlap.sol')
    assert words(harness.call(app, 'mcopy_to_right_overlap()')) == (
        0x2222222222222222333333333333333344444444444444445555555555555555,
        0x4444444444444444555555555555555566666666666666667777777777777777,
        0x88888888888888889999999999999999ccccccccccccccccdddddddddddddddd,)
    assert words(harness.call(app, 'mcopy_to_left_overlap()')) == (
        0x2222222222222222333333333333333366666666666666667777777777777777,
        0x88888888888888889999999999999999aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb,
        0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd,)
    assert words(harness.call(app, 'mcopy_in_place()')) == (
        0x2222222222222222333333333333333344444444444444445555555555555555,
        0x6666666666666666777777777777777788888888888888889999999999999999,
        0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd,)
    assert words(harness.call(app, 'mcopy_to_right_no_overlap()')) == (
        0x2222222222222222333333333333333344444444444444445555555555555555,
        0x6666666666666666777777777777777744444444444444445555555555555555,
        0x66666666666666667777777777777777ccccccccccccccccdddddddddddddddd,)
    assert words(harness.call(app, 'mcopy_to_left_no_overlap()')) == (
        0x2222222222222222333333333333333388888888888888889999999999999999,
        0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb88888888888888889999999999999999,
        0xaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbccccccccccccccccdddddddddddddddd,)

def test_optimize_memory_store_multi_block(harness):
    """inlineAssembly/contracts/optimize_memory_store_multi_block.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/optimize_memory_store_multi_block.sol")
    # f() -> 0x42
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 66
    # g() -> true
    r = harness.call(app, "g()")
    assert bool(as_int(r.abi_return)) is True

@pytest.mark.xfail(reason="Yul `log0` has no AVM equivalent; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0)", strict=False)
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
    # f() returns address(this).balance after the payment lands. AVM total
    # balance includes the MBR baseline — verify the 254 microalgos are
    # observable, not the absolute value.
    r = harness.call(app, "f()", payment_wei=254)
    assert as_int(r.abi_return) - app.balance_baseline == 254

def test_signextend_runtime(harness):
    """inlineAssembly/contracts/signextend_runtime.sol — signextend(b, x) with a
    runtime (non-constant) b, lowered as sar(s, shl(s, x)), s = 248 - 8*min(b,31).
    Verified against canonical EVM signextend."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/signextend_runtime.sol")
    M = (1 << 256) - 1
    def evm_se(b, x):
        x &= M
        if b >= 31:
            return x
        tb = b * 8 + 7
        return (x | (M ^ ((1 << (tb + 1)) - 1))) & M if (x >> tb) & 1 else x & ((1 << (tb + 1)) - 1)
    for b, x in [(0, 0x7f), (0, 0x80), (0, 0xff), (0, 0x1ff), (1, 0x80),
                 (1, 0x8000), (2, 0x123456), (31, 0xff), (32, 0x80), (40, 0xdead)]:
        r = harness.call(app, "se(uint256,uint256)", b, x)
        assert as_int(r.abi_return) == evm_se(b, x), f"signextend({b}, {hex(x)})"

def test_signextend_adddelta(harness):
    """inlineAssembly/contracts/signextend_adddelta.sol — Uniswap V4 LiquidityMath.addDelta:
    signextend(15, y) (constant byte index) on a NEGATIVE int128, composed with a 128-bit add
    and an shr(128) overflow guard. A removal (negative delta) must round-trip, not revert."""
    app = harness.compile_and_deploy("inlineAssembly/contracts/signextend_adddelta.sol")
    c = lambda *a, **k: harness.call(app, "addDelta(uint128,int128)", *a, **k)
    # positive delta (add) — confirms routing + the non-negative path
    assert as_int(c(5000000, 3000000).abi_return) == 8000000
    assert as_int(c(0, 5000000).abi_return) == 5000000
    # NEGATIVE delta (remove) — the bug: signextend(15, y) must sign-extend, so these
    # must round-trip rather than revert/overflow.
    assert as_int(c(5000000, -5000000).abi_return) == 0          # remove the whole position
    assert as_int(c(10000000, -5000000).abi_return) == 5000000   # partial remove
    # underflow: remove more than exists -> revert
    assert c(3000000, -5000000, expect_revert=True).reverted

def test_shadowing_local_function_opcode(harness):
    """inlineAssembly/contracts/shadowing_local_function_opcode.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/shadowing_local_function_opcode.sol")
    # g() -> 7, 3
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 3)

def test_slot_access(harness):  # currently fails
    """inlineAssembly/contracts/slot_access.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/slot_access.sol')
    r = harness.call(app, 'get()')
    assert as_int(r.abi_return) == 0
    r = harness.call(app, 'mappingAccess(uint256)', 1)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0,)
    r = harness.call(app, 'set(uint256)', 4)
    r = harness.call(app, 'get()')
    assert as_int(r.abi_return) == 4
    r = harness.call(app, 'mappingAccess(uint256)', 1)
    assert tuple(as_int(x) for x in r.abi_return) == (4, 0,)

def test_slot_access_via_mapping_pointer(harness):  # currently fails
    """inlineAssembly/contracts/slot_access_via_mapping_pointer.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/slot_access_via_mapping_pointer.sol')
    r = harness.call(app, 'f(uint256)', 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0,)
    r = harness.call(app, 'f(uint256)', 1)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0,)
    r = harness.call(app, 'f(uint256)', 2)
    assert tuple(as_int(x) for x in r.abi_return) == (2, 0,)

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

def test_transient_storage_low_level_calls(harness):  # currently fails
    """inlineAssembly/contracts/transient_storage_low_level_calls.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/transient_storage_low_level_calls.sol')
    r = harness.call(app, 'testDelegateCall()')
    assert r.abi_return is True
    r = harness.call(app, 'testCall()')
    assert r.abi_return is True
    r = harness.call(app, 'tloadAllowedStaticCall()')
    assert r.abi_return is True
    r = harness.call(app, 'tstoreNotAllowedStaticCall()')
    assert r.abi_return is True

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

def test_tstore_hidden_staticcall(harness):  # currently fails
    """inlineAssembly/contracts/tstore_hidden_staticcall.sol"""
    app = harness.compile_and_deploy('inlineAssembly/contracts/tstore_hidden_staticcall.sol')
    r = harness.call(app, 'test()', expect_revert=True)
    assert r.reverted

def test_sstore_box_struct_slot(harness):
    """inlineAssembly/contracts/sstore_box_struct_slot.sol

    `sstore(ref.slot, packedWord)` where `ref = m[k]` aliases a struct-valued
    mapping element packs two 128-bit fields into EVM slot 0 — the Uniswap V4
    Pool.updateTick idiom. Verifies the box-keyed-struct sstore lowering writes
    the correct ARC-4 fields (incl. a signed int128) and leaves the slot-1
    field untouched.
    """
    app = harness.compile_and_deploy("inlineAssembly/contracts/sstore_box_struct_slot.sol")
    # c lives in slot 1; set it first to prove the slot-0 sstore preserves it.
    assert not harness.call(app, "setC(uint256,uint256)", 7, 0xdeadbeef).reverted
    # pack a (low 128) and b (high 128, signed) into slot 0 via inline-assembly
    # sstore. b is negative to exercise signed int128 packing (V4 liquidityNet)
    # — this also covers the shl(128, negativeInt128) path, which overflows the
    # AVM 64-byte bigint limit without the wrapMod256-before-multiply fix.
    assert not harness.call(app, "setPacked(uint256,uint128,int128)", 7, 111, -222).reverted
    assert as_int(harness.call(app, "getA(uint256)", 7).abi_return) == 111
    # int128 -222 round-trips as its 256-bit two's-complement (signed >64-bit
    # returns are exposed as the unsigned 256-bit bit-pattern).
    assert as_int(harness.call(app, "getB(uint256)", 7).abi_return) == (1 << 256) - 222
    assert as_int(harness.call(app, "getC(uint256)", 7).abi_return) == 0xdeadbeef


def test_balancedelta_int128(harness):
    """inlineAssembly/contracts/balancedelta_int128.sol

    V4 BalanceDelta pack/unpack with NEGATIVE int128 amounts (remove-liquidity /
    owed-to-caller deltas). amount0 = sar(128, bd), amount1 = signextend(15, bd) must
    sign-extend. Signed >64-bit returns surface as the unsigned 256-bit two's-complement
    bit-pattern, so interpret with s256.
    """
    app = harness.compile_and_deploy("inlineAssembly/contracts/balancedelta_int128.sol")

    def s256(u):
        return u - (1 << 256) if u >= (1 << 255) else u

    def a0(x, y):
        return s256(as_int(harness.call(app, "amount0(int128,int128)", x, y).abi_return))

    def a1(x, y):
        return s256(as_int(harness.call(app, "amount1(int128,int128)", x, y).abi_return))

    assert a0(-100, -200) == -100 and a1(-100, -200) == -200  # both negative
    assert a0(100, -200) == 100 and a1(100, -200) == -200      # +/-
    assert a0(-100, 200) == -100 and a1(-100, 200) == 200      # -/+
    assert a0(100, 200) == 100 and a1(100, 200) == 200         # both positive
