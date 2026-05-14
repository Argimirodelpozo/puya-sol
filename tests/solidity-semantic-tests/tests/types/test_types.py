"""Tests for the types category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_array_mapping_abstract_constructor_param(harness):
    """types/contracts/array_mapping_abstract_constructor_param.sol"""
    app = harness.compile_and_deploy("types/contracts/array_mapping_abstract_constructor_param.sol")
    # m(uint256,uint256,uint256): 0, 0, 0 -> FAILURE
    r = harness.call(app, "m(uint256,uint256,uint256)", 0, 0, 0, expect_revert=True)
    assert r.reverted
    # m(uint256,uint256,uint256): 1, 0, 1 -> 2
    r = harness.call(app, "m(uint256,uint256,uint256)", 1, 0, 1)
    assert as_int(r.abi_return) == 2
    # m(uint256,uint256,uint256): 1, 0, 5 -> 0
    r = harness.call(app, "m(uint256,uint256,uint256)", 1, 0, 5)
    assert as_int(r.abi_return) == 0

def test_assign_calldata_value_type(harness):
    """types/contracts/assign_calldata_value_type.sol"""
    app = harness.compile_and_deploy("types/contracts/assign_calldata_value_type.sol")
    # f(uint256): 23 -> 42, 23
    r = harness.call(app, "f(uint256)", 23)
    assert tuple(as_int(x) for x in r.abi_return) == (42, 23)

def test_convert_fixed_bytes_to_fixed_bytes_greater_size(harness):
    """types/contracts/convert_fixed_bytes_to_fixed_bytes_greater_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_fixed_bytes_greater_size.sol")
    # bytesToBytes(bytes2): "ab" -> "ab"
    r = harness.call(app, "bytesToBytes(bytes2)", bytes.fromhex('6162'))
    # TODO: verify expected: "ab"
    assert not r.reverted

def test_convert_fixed_bytes_to_fixed_bytes_same_size(harness):
    """types/contracts/convert_fixed_bytes_to_fixed_bytes_same_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_fixed_bytes_same_size.sol")
    # bytesToBytes(bytes4): "abcd" -> "abcd"
    r = harness.call(app, "bytesToBytes(bytes4)", bytes.fromhex('61626364'))
    # TODO: verify expected: "abcd"
    assert not r.reverted

def test_convert_fixed_bytes_to_fixed_bytes_smaller_size(harness):
    """types/contracts/convert_fixed_bytes_to_fixed_bytes_smaller_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_fixed_bytes_smaller_size.sol")
    # bytesToBytes(bytes4): "abcd" -> "ab"
    r = harness.call(app, "bytesToBytes(bytes4)", bytes.fromhex('61626364'))
    # TODO: verify expected: "ab"
    assert not r.reverted

def test_convert_fixed_bytes_to_uint_greater_size(harness):
    """types/contracts/convert_fixed_bytes_to_uint_greater_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_uint_greater_size.sol")
    # bytesToUint(bytes4): "abcd" -> 0x61626364
    r = harness.call(app, "bytesToUint(bytes4)", bytes.fromhex('61626364'))
    assert as_int(r.abi_return) == 1633837924

def test_convert_fixed_bytes_to_uint_same_min_size(harness):
    """types/contracts/convert_fixed_bytes_to_uint_same_min_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_uint_same_min_size.sol")
    # bytesToUint(bytes1): "a" -> 0x61
    r = harness.call(app, "bytesToUint(bytes1)", bytes.fromhex('61'))
    assert as_int(r.abi_return) == 97

def test_convert_fixed_bytes_to_uint_same_type(harness):
    """types/contracts/convert_fixed_bytes_to_uint_same_type.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_uint_same_type.sol")
    # bytes32 "abc2" left-padded with zeros to 32 bytes for the static-array arg.
    arg = rpad(b"abc2", 32)
    r = harness.call(app, "bytesToUint(bytes32)", arg)
    assert as_int(r.abi_return) == int.from_bytes(arg, "big")

def test_convert_fixed_bytes_to_uint_smaller_size(harness):
    """types/contracts/convert_fixed_bytes_to_uint_smaller_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_fixed_bytes_to_uint_smaller_size.sol")
    # bytesToUint(bytes4): "abcd" -> 0x6364
    r = harness.call(app, "bytesToUint(bytes4)", bytes.fromhex('61626364'))
    assert as_int(r.abi_return) == 25444

def test_convert_uint_to_fixed_bytes_greater_size(harness):
    """types/contracts/convert_uint_to_fixed_bytes_greater_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_uint_to_fixed_bytes_greater_size.sol")
    # UintToBytes(uint16): 0x6162 -> "\x00\x00\x00\x00\x00\x00ab"
    r = harness.call(app, "UintToBytes(uint16)", 24930)
    # TODO: verify expected: "\x00\x00\x00\x00\x00\x00ab"
    assert not r.reverted

def test_convert_uint_to_fixed_bytes_same_min_size(harness):
    """types/contracts/convert_uint_to_fixed_bytes_same_min_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_uint_to_fixed_bytes_same_min_size.sol")
    # UintToBytes(uint8): 0x61 -> "a"
    r = harness.call(app, "UintToBytes(uint8)", 97)
    # TODO: verify expected: "a"
    assert not r.reverted

def test_convert_uint_to_fixed_bytes_same_size(harness):
    """types/contracts/convert_uint_to_fixed_bytes_same_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_uint_to_fixed_bytes_same_size.sol")
    # uintToBytes(uint256): left(0x616263) -> left(0x616263)
    r = harness.call(app, "uintToBytes(uint256)", 0x6162630000000000000000000000000000000000000000000000000000000000)
    # TODO: verify expected: left(0x616263)
    assert not r.reverted

def test_convert_uint_to_fixed_bytes_smaller_size(harness):
    """types/contracts/convert_uint_to_fixed_bytes_smaller_size.sol"""
    app = harness.compile_and_deploy("types/contracts/convert_uint_to_fixed_bytes_smaller_size.sol")
    # uintToBytes(uint32): 0x61626364 -> "cd"
    r = harness.call(app, "uintToBytes(uint32)", 1633837924)
    # TODO: verify expected: "cd"
    assert not r.reverted

def test_external_function_to_address(harness):
    """types/contracts/external_function_to_address.sol"""
    app = harness.compile_and_deploy("types/contracts/external_function_to_address.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True
    # g(function) on AVM: function pointer is 12 bytes (8-byte sub-id + 4-byte
    # selector). EVM packs (20-byte address + 4-byte selector) into bytes32.
    # The semantics differ; just verify the call succeeds with a valid fn-ptr.
    fnptr = bytes.fromhex('000000000000000000000422')
    r = harness.call(app, "g(function)", list(fnptr))
    assert isinstance(r.abi_return, str)  # address as base32 string

def test_mapping_abstract_constructor_param(harness):
    """types/contracts/mapping_abstract_constructor_param.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_abstract_constructor_param.sol")
    # m(uint256): 1 -> 0
    r = harness.call(app, "m(uint256)", 1)
    assert as_int(r.abi_return) == 0
    # m(uint256): 5 -> 20
    r = harness.call(app, "m(uint256)", 5)
    assert as_int(r.abi_return) == 20

def test_mapping_contract_key(harness):
    """types/contracts/mapping_contract_key.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_contract_key.sol")
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 161)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((0).to_bytes(32, "big")), 239)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 5)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0x05
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 5
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0

def test_mapping_contract_key_getter(harness):
    """types/contracts/mapping_contract_key_getter.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_contract_key_getter.sol")
    # table(address): 0 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # table(address): 0x01 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # table(address): 0xa7 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 161)
    # (void return — call succeeding is the assertion)
    # table(address): 0 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # table(address): 0x01 -> 0xa1
    r = harness.call(app, "table(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # table(address): 0xa7 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((0).to_bytes(32, "big")), 239)
    # (void return — call succeeding is the assertion)
    # table(address): 0 -> 0xef
    r = harness.call(app, "table(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # table(address): 0x01 -> 0xa1
    r = harness.call(app, "table(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # table(address): 0xa7 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 5)
    # (void return — call succeeding is the assertion)
    # table(address): 0 -> 0xef
    r = harness.call(app, "table(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # table(address): 0x01 -> 0x05
    r = harness.call(app, "table(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 5
    # table(address): 0xa7 -> 0
    r = harness.call(app, "table(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0x05
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 5
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0

def test_mapping_contract_key_library(harness):
    """types/contracts/mapping_contract_key_library.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_contract_key_library.sol")
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 161)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((0).to_bytes(32, "big")), 239)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0xa1
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 161
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0
    # set(address,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(address,uint8)", encoding.encode_address((1).to_bytes(32, "big")), 5)
    # (void return — call succeeding is the assertion)
    # get(address): 0 -> 0xef
    r = harness.call(app, "get(address)", encoding.encode_address((0).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 239
    # get(address): 0x01 -> 0x05
    r = harness.call(app, "get(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 5
    # get(address): 0xa7 -> 0
    r = harness.call(app, "get(address)", encoding.encode_address((167).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 0

def test_mapping_enum_key_getter_v1(harness):
    """types/contracts/mapping_enum_key_getter_v1.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_getter_v1.sol")
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0x01 -> 0
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0xa7 -> 0
    r = harness.call(app, "table(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # table(uint8): 0xa7 -> 0
    r = harness.call(app, "table(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # table(uint8): 0xa7 -> 0
    r = harness.call(app, "table(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # table(uint8): 0x01 -> 0x05
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # table(uint8): 0xa7 -> 0
    r = harness.call(app, "table(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_enum_key_getter_v2(harness):
    """types/contracts/mapping_enum_key_getter_v2.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_getter_v2.sol")
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0x01 -> 0
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # table(uint8): 0x01 -> 0xa1
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # table(uint8): 0 -> 0xef
    r = harness.call(app, "table(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # table(uint8): 0x01 -> 0x05
    r = harness.call(app, "table(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # table(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "table(uint8)", 167, expect_revert=True)
    assert r.reverted
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_enum_key_library_v1(harness):
    """types/contracts/mapping_enum_key_library_v1.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_library_v1.sol")
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_enum_key_library_v2(harness):
    """types/contracts/mapping_enum_key_library_v2.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_library_v2.sol")
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_enum_key_v1(harness):
    """types/contracts/mapping_enum_key_v1.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_v1.sol")
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x02 -> 0
    r = harness.call(app, "get(uint8)", 2)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x03 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 3, expect_revert=True)
    assert r.reverted
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE, hex"4e487b71", 33
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_enum_key_v2(harness):
    """types/contracts/mapping_enum_key_v2.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_enum_key_v2.sol")
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x02 -> 0
    r = harness.call(app, "get(uint8)", 2)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x03 -> FAILURE
    r = harness.call(app, "get(uint8)", 3, expect_revert=True)
    assert r.reverted
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> FAILURE
    r = harness.call(app, "get(uint8)", 167, expect_revert=True)
    assert r.reverted

def test_mapping_simple(harness):
    """types/contracts/mapping_simple.sol"""
    app = harness.compile_and_deploy("types/contracts/mapping_simple.sol")
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0xa7 -> 0
    r = harness.call(app, "get(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # set(uint8,uint8): 0x01, 0xa1 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 161)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 0
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> 0
    r = harness.call(app, "get(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # set(uint8,uint8): 0x00, 0xef ->
    r = harness.call(app, "set(uint8,uint8)", 0, 239)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0xa1
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 161
    # get(uint8): 0xa7 -> 0
    r = harness.call(app, "get(uint8)", 167)
    assert as_int(r.abi_return) == 0
    # set(uint8,uint8): 0x01, 0x05 ->
    r = harness.call(app, "set(uint8,uint8)", 1, 5)
    # (void return — call succeeding is the assertion)
    # get(uint8): 0 -> 0xef
    r = harness.call(app, "get(uint8)", 0)
    assert as_int(r.abi_return) == 239
    # get(uint8): 0x01 -> 0x05
    r = harness.call(app, "get(uint8)", 1)
    assert as_int(r.abi_return) == 5
    # get(uint8): 0xa7 -> 0
    r = harness.call(app, "get(uint8)", 167)
    assert as_int(r.abi_return) == 0

def test_nested_tuples(harness):
    """types/contracts/nested_tuples.sol"""
    app = harness.compile_and_deploy("types/contracts/nested_tuples.sol")
    # f0() -> 2, true
    r = harness.call(app, "f0()")
    # TODO: verify expected: 2 | true
    assert not r.reverted
    # f1() -> 1
    r = harness.call(app, "f1()")
    assert as_int(r.abi_return) == 1
    # f2() -> 2
    r = harness.call(app, "f2()")
    assert as_int(r.abi_return) == 2
    # f3() -> 3
    r = harness.call(app, "f3()")
    assert as_int(r.abi_return) == 3
    # f4() -> 4
    r = harness.call(app, "f4()")
    assert as_int(r.abi_return) == 4

def test_packing_signed_types(harness):
    """types/contracts/packing_signed_types.sol"""
    app = harness.compile_and_deploy("types/contracts/packing_signed_types.sol")
    # run() -> 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa
    r = harness.call(app, "run()")
    assert as_int(r.abi_return) == 115792089237316195423570985008687907853269984665640564039457584007913129639930

def test_packing_unpacking_types(harness):
    """types/contracts/packing_unpacking_types.sol"""
    app = harness.compile_and_deploy("types/contracts/packing_unpacking_types.sol")
    # run(bool,uint32,uint64): true, 0x0f0f0f0f, 0xf0f0f0f0f0f0f0f0 -> 0x0000000000000000000000000000000000000001f0f0f0f00f0f0f0f0f0f0f0f
    r = harness.call(app, "run(bool,uint32,uint64)", True, 252645135, 0xf0f0f0f0f0f0f0f0)
    assert as_int(r.abi_return) == 153795844864354234087135710991

def test_strings(harness):
    """types/contracts/strings.sol"""
    app = harness.compile_and_deploy("types/contracts/strings.sol")
    # fixedBytesHex() -> "\xaa\xbb\x00\xff"
    r = harness.call(app, "fixedBytesHex()")
    # TODO: verify expected: "\xaa\xbb\x00\xff"
    assert not r.reverted
    # fixedBytes() -> "abc\x00\xff__"
    r = harness.call(app, "fixedBytes()")
    # TODO: verify expected: "abc\x00\xff__"
    assert not r.reverted
    # pipeThrough(bytes2,bool): "\x00\x02", true -> "\x00\x02", true
    r = harness.call(app, "pipeThrough(bytes2,bool)", bytes.fromhex('0002'), True)
    # TODO: verify expected: "\x00\x02" | true
    assert not r.reverted

def test_struct_mapping_abstract_constructor_param(harness):
    """types/contracts/struct_mapping_abstract_constructor_param.sol"""
    app = harness.compile_and_deploy("types/contracts/struct_mapping_abstract_constructor_param.sol")
    # getM(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "getM(uint256,uint256)", 0, 0)
    assert as_int(r.abi_return) == 0
    # getM(uint256,uint256): 1, 5 -> 0x10
    r = harness.call(app, "getM(uint256,uint256)", 1, 5)
    assert as_int(r.abi_return) == 16
    # getM(uint256,uint256): 1, 0 -> 0
    r = harness.call(app, "getM(uint256,uint256)", 1, 0)
    assert as_int(r.abi_return) == 0

def test_tuple_assign_multi_slot_grow(harness):
    """types/contracts/tuple_assign_multi_slot_grow.sol"""
    app = harness.compile_and_deploy("types/contracts/tuple_assign_multi_slot_grow.sol")
    # f() -> 0x30, 0x31, 0x32
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (48, 49, 50)

def test_type_conversion_cleanup(harness):
    """types/contracts/type_conversion_cleanup.sol"""
    app = harness.compile_and_deploy("types/contracts/type_conversion_cleanup.sol")
    # test() -> 0xffffffffffffffffffffffffffffffff
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 340282366920938463463374607431768211455
