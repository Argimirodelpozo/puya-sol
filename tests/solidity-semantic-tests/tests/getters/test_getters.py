"""Tests for the getters category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_array_mapping_struct(harness):
    """getters/contracts/array_mapping_struct.sol"""
    pytest.fail("Compiler-side: auto-getter on mapping(uint=>Struct[]) — codegen exits 1.")

def test_arrays(harness):
    """getters/contracts/arrays.sol — `uint8[][2] public a; a[1].push(N)`
    auto-getter. The 2D state-array `arr[i].push()` codegen now unwraps
    inner StateGet inside the IndexExpression chain so puya accepts the
    write target.
    """
    app = harness.compile_and_deploy("getters/contracts/arrays.sol")
    # a(uint, uint): 0, 0 -> out of bounds (a[0] is empty)
    assert harness.call(app, "a(uint256,uint256)", 0, 0, expect_revert=True).reverted
    # a(uint, uint): 1, 0 -> 3
    assert as_int(harness.call(app, "a(uint256,uint256)", 1, 0).abi_return) == 3
    # a(uint, uint): 1, 1 -> 4
    assert as_int(harness.call(app, "a(uint256,uint256)", 1, 1).abi_return) == 4
    # a(uint, uint): 2, 0 -> out of bounds (static array has only 2 elements)
    assert harness.call(app, "a(uint256,uint256)", 2, 0, expect_revert=True).reverted

def test_bytes(harness):
    """getters/contracts/bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/bytes.sol")
    # Public bytes state-var getter returns the raw bytes value.
    assert bytes(harness.call(app, "b()").abi_return) == b"abc"

def test_mapping(harness):
    """getters/contracts/mapping.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping.sol")
    # x(uint256,uint256): 1, 2 -> 3
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert as_int(r.abi_return) == 3
    # x(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert as_int(r.abi_return) == 0

def test_mapping_array_struct(harness):
    """getters/contracts/mapping_array_struct.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_array_struct.sol")
    # m(uint256,uint256): 0, 0 -> FAILURE
    r = harness.call(app, "m(uint256,uint256)", 0, 0, expect_revert=True)
    assert r.reverted
    # m(uint256,uint256): 1, 0 -> 1, 2
    r = harness.call(app, "m(uint256,uint256)", 1, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # m(uint256,uint256): 1, 1 -> 3, 4
    r = harness.call(app, "m(uint256,uint256)", 1, 1)
    assert tuple(as_int(x) for x in r.abi_return) == (3, 4)
    # m(uint256,uint256): 1, 2 -> FAILURE
    r = harness.call(app, "m(uint256,uint256)", 1, 2, expect_revert=True)
    assert r.reverted
    # n(uint256,uint256): 0, 0 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # n(uint256,uint256): 1, 0 -> 7, 8
    r = harness.call(app, "n(uint256,uint256)", 1, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)
    # n(uint256,uint256): 1, 1 -> 9, 0x0a
    r = harness.call(app, "n(uint256,uint256)", 1, 1)
    assert tuple(as_int(x) for x in r.abi_return) == (9, 10)
    # n(uint256,uint256): 1, 2 -> 0x00, 0x00
    r = harness.call(app, "n(uint256,uint256)", 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)

def test_mapping_of_string(harness):
    """getters/contracts/mapping_of_string.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_of_string.sol")
    # Public mapping getter `x(key, index)` returns x[key][index].
    assert as_int(harness.call(app, "x(string,uint256)", "abc", 0).abi_return) == 1
    assert as_int(harness.call(app, "x(string,uint256)", "abc", 1).abi_return) == 2
    assert as_int(harness.call(app, "x(string,uint256)", "abc", 2).abi_return) == 3
    assert as_int(harness.call(app, "x(string,uint256)", "def", 0).abi_return) == 0
    assert as_int(harness.call(app, "x(string,uint256)", "def", 1).abi_return) == 9
    assert as_int(harness.call(app, "x(string,uint256)", "def", 2).abi_return) == 0


def test_mapping_to_struct(harness):
    """getters/contracts/mapping_to_struct.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_to_struct.sol")
    # x(uint256,uint256): 1, 2 -> 3, 4, 5, 6
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (3, 4, 5, 6)
    # x(uint256,uint256): 0, 0 -> 0x00, 0x00, 0x00, 0x00
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)

def test_mapping_with_names(harness):
    """getters/contracts/mapping_with_names.sol"""
    app = harness.compile_and_deploy("getters/contracts/mapping_with_names.sol")
    # x(uint256,uint256): 1, 2 -> 3
    r = harness.call(app, "x(uint256,uint256)", 1, 2)
    assert as_int(r.abi_return) == 3
    # x(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "x(uint256,uint256)", 0, 0)
    assert as_int(r.abi_return) == 0

def test_string_and_bytes(harness):
    """getters/contracts/string_and_bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/string_and_bytes.sol")
    # Public string/bytes state-var getters. algosdk decodes string as str
    # and bytes as list[int].
    assert harness.call(app, "a()").abi_return == "hello world"
    assert harness.call(app, "b()").abi_return == "ABCD"
    assert bytes(harness.call(app, "c()").abi_return) == b"\xff\x07\x7f\xff"
    assert harness.call(app, "d()").abi_return == "abcd"

def test_struct_with_bytes(harness):
    """getters/contracts/struct_with_bytes.sol"""
    app = harness.compile_and_deploy("getters/contracts/struct_with_bytes.sol")
    # Public struct getter for S{a, b, ... } returns the value-type fields
    # (a, b) — the mapping/array fields don't appear in the auto-generated getter.
    r = harness.call(app, "s()")
    assert as_int(r.abi_return[0]) == 7
    assert bytes(r.abi_return[1]) == b"abc"


def test_struct_with_bytes_simple(harness):
    """getters/contracts/struct_with_bytes_simple.sol"""
    app = harness.compile_and_deploy("getters/contracts/struct_with_bytes_simple.sol")
    r = harness.call(app, "s()")
    assert as_int(r.abi_return[0]) == 7
    assert bytes(r.abi_return[1]) == b"abc"

def test_transient_value_types(harness):
    """getters/contracts/transient_value_types.sol"""
    app = harness.compile_and_deploy("getters/contracts/transient_value_types.sol")
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0
    # f() -> -1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # x() -> 0
    r = harness.call(app, "x()")
    assert as_int(r.abi_return) == 0

def test_transient_value_types_multi_frame_call(harness):
    """getters/contracts/transient_value_types_multi_frame_call.sol"""
    pytest.fail("EVM transient storage persists across multi-frame external calls within one tx. AVM transient (scratch) wipes between inner app calls — different semantic model.")

def test_value_types(harness):
    """getters/contracts/value_types.sol"""
    from algosdk import encoding
    app = harness.compile_and_deploy("getters/contracts/value_types.sol")
    assert as_int(harness.call(app, "a()").abi_return) == 3
    assert as_int(harness.call(app, "b()").abi_return) == 4
    assert as_int(harness.call(app, "c()").abi_return) == 5
    assert as_int(harness.call(app, "d()").abi_return) == 6
    # Fixed-byte getters: bytes1 / bytes20 / bytes32 return their raw bytes.
    assert bytes(harness.call(app, "e()").abi_return) == b"\x7f"
    assert bytes(harness.call(app, "f()").abi_return) == bytes.fromhex("6465616462656566313564656164000000000010")
    assert bytes(harness.call(app, "g()").abi_return) == bytes.fromhex("6465616462656566313564656164000000000000000000000000000000000010")
    assert harness.call(app, "h()").abi_return is True
    # `i` is an `address` — algosdk returns it as a 58-char algorand address;
    # decode to compare against the 20-byte EVM value (zero-padded to 32 by AVM).
    addr_str = harness.call(app, "i()").abi_return
    raw = encoding.decode_address(addr_str)
    assert int.from_bytes(raw, "big") == 0x5555555555555555555555555555555555555555
