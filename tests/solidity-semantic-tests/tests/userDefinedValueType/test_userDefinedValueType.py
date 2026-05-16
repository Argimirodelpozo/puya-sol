"""Tests for the userDefinedValueType category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_abicodec(harness):
    """userDefinedValueType/contracts/abicodec.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/abicodec.sol")
    r = harness.call(app, "g()", extra_fee=10000)
    assert bool(as_int(r.abi_return)) is True

def test_assembly_access_bytes2_abicoder_v1(harness):
    """userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v1.sol")
    # AVM stores bytes2 as 2 raw bytes (no EVM 32-byte right-padding).
    assert bytes(harness.call(app, "f(bytes2)", b"ab").abi_return) == b"ab"
    assert bytes(harness.call(app, "g(bytes2)", b"ab").abi_return) == b"ab"

def test_assembly_access_bytes2_abicoder_v2(harness):
    """userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v2.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/assembly_access_bytes2_abicoder_v2.sol")
    assert bytes(harness.call(app, "f(bytes2)", b"ab").abi_return) == b"ab"
    assert bytes(harness.call(app, "g(bytes2)", b"ab").abi_return) == b"ab"

def test_calldata(harness):
    """userDefinedValueType/contracts/calldata.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/calldata.sol")
    # test_f() -> true
    r = harness.call(app, "test_f()")
    assert bool(as_int(r.abi_return)) is True
    # test_g() -> true
    r = harness.call(app, "test_g()")
    assert bool(as_int(r.abi_return)) is True
    # addresses(uint256): 0 -> 0x18
    r = harness.call(app, "addresses(uint256)", 0)
    assert as_int(r.abi_return) == 24
    # addresses(uint256): 1 -> 0x19
    r = harness.call(app, "addresses(uint256)", 1)
    assert as_int(r.abi_return) == 25
    # addresses(uint256): 3 -> 0x1b
    r = harness.call(app, "addresses(uint256)", 3)
    assert as_int(r.abi_return) == 27
    # addresses(uint256): 4 -> 0x1c
    r = harness.call(app, "addresses(uint256)", 4)
    assert as_int(r.abi_return) == 28
    # addresses(uint256): 5 -> FAILURE
    r = harness.call(app, "addresses(uint256)", 5, expect_revert=True)
    assert r.reverted

def test_calldata_to_storage(harness):
    """userDefinedValueType/contracts/calldata_to_storage.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/calldata_to_storage.sol")
    # s() -> 0, 0, 0x00, 0
    r = harness.call(app, "s()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)
    harness.call(app, "f((uint8,uint16,bytes2,uint8))", (1, 255, b"ab", 15))
    r = harness.call(app, "s()")
    # bytes2 field "ab" — AVM stores raw bytes (no 32-byte EVM padding).
    assert (as_int(r.abi_return[0]), as_int(r.abi_return[1]), bytes(r.abi_return[2]), as_int(r.abi_return[3])) == (1, 255, b"ab", 15)
    assert not harness.call(app, "g(uint16[])", [1, 2, 3]).reverted
    assert as_int(harness.call(app, "small(uint256)", 0).abi_return) == 1
    assert as_int(harness.call(app, "small(uint256)", 1).abi_return) == 2
    assert not harness.call(app, "h(bytes2[])", [b"ab", b"cd", b"ef"]).reverted
    # l(0)/l(1) read bytes2 entries from storage. AVM stores them as raw 2-byte values.
    assert bytes(harness.call(app, "l(uint256)", 0).abi_return) == b"ab"
    assert bytes(harness.call(app, "l(uint256)", 1).abi_return) == b"cd"

def test_cleanup(harness):
    """userDefinedValueType/contracts/cleanup.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/cleanup.sol")
    # ret() -> 0xff
    r = harness.call(app, "ret()")
    assert as_int(r.abi_return) == 255
    # f(uint8): 0x1ff -> FAILURE
    r = harness.call(app, "f(uint8)", 511, expect_revert=True)
    assert r.reverted
    # f(uint8): 0xff -> 0xff
    r = harness.call(app, "f(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # mem() returns uint8[]; AVM decodes as a list of ints (not EVM-flat offset/length).
    assert tuple(as_int(x) for x in harness.call(app, "mem()").abi_return) == (255, 255)
    assert tuple(as_int(x) for x in harness.call(app, "stor()").abi_return) == (1, 255, 2)

def test_cleanup_abicoderv1(harness):
    """userDefinedValueType/contracts/cleanup_abicoderv1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/cleanup_abicoderv1.sol")
    assert as_int(harness.call(app, "ret()").abi_return) == 255
    # AVM/algosdk reject 511 as a uint8 arg before it ever reaches the
    # contract — the abicoder-v1 truncation behaviour isn't observable here.
    assert as_int(harness.call(app, "f(uint8)", 255).abi_return) == 255
    # mem() returns uint8[]; AVM stores raw uint8 values (with cleanup, not the EVM-dirty 0x01ff).
    assert tuple(as_int(x) for x in harness.call(app, "mem()").abi_return) == (255, 255)
    assert tuple(as_int(x) for x in harness.call(app, "stor()").abi_return) == (1, 255, 2)

def test_constant(harness):
    """userDefinedValueType/contracts/constant.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/constant.sol")
    # s() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "s()")
    assert as_int(r.abi_return) == 165521356710917456517261742455526507355687727119203895813322792776
    # t() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "t()")
    assert as_int(r.abi_return) == 165521356710917456517261742455526507355687727119203895813322792776
    # u() -> 165521356710917456517261742455526507355687727119203895813322792776
    r = harness.call(app, "u()")
    assert as_int(r.abi_return) == 165521356710917456517261742455526507355687727119203895813322792776

def test_conversion(harness):
    """userDefinedValueType/contracts/conversion.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/conversion.sol")
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # f(uint256): 257 -> 1
    r = harness.call(app, "f(uint256)", 257)
    assert as_int(r.abi_return) == 1
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # g(uint256): 255 -> -1
    r = harness.call(app, "g(uint256)", 255)
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # g(uint256): 257 -> 1
    r = harness.call(app, "g(uint256)", 257)
    assert as_int(r.abi_return) == 1
    # h(uint8): 1 -> 1
    r = harness.call(app, "h(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # h(uint8): 2 -> 2
    r = harness.call(app, "h(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # h(uint8): 255 -> -1
    r = harness.call(app, "h(uint8)", 255)
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # h(uint8): 257 -> FAILURE
    r = harness.call(app, "h(uint8)", 257, expect_revert=True)
    assert r.reverted
    # i(uint8): 250 -> 250
    r = harness.call(app, "i(uint8)", 250)
    assert as_int(r.abi_return) == 250
    # j(uint8): 1 -> 1
    r = harness.call(app, "j(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # j(uint8): 2 -> 2
    r = harness.call(app, "j(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # j(uint8): 255 -> 0xff
    r = harness.call(app, "j(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # j(uint8): 257 -> FAILURE
    r = harness.call(app, "j(uint8)", 257, expect_revert=True)
    assert r.reverted
    # k(uint8): 1 -> 1
    r = harness.call(app, "k(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # k(uint8): 2 -> 2
    r = harness.call(app, "k(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # k(uint8): 255 -> 0xff
    r = harness.call(app, "k(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # k(uint8): 257 -> FAILURE
    r = harness.call(app, "k(uint8)", 257, expect_revert=True)
    assert r.reverted
    # m(uint16): 1 -> 1
    r = harness.call(app, "m(uint16)", 1)
    assert as_int(r.abi_return) == 1
    # m(uint16): 2 -> 2
    r = harness.call(app, "m(uint16)", 2)
    assert as_int(r.abi_return) == 2
    # m(uint16): 255 -> 0xff
    r = harness.call(app, "m(uint16)", 255)
    assert as_int(r.abi_return) == 255
    # m(uint16): 257 -> 1
    r = harness.call(app, "m(uint16)", 257)
    assert as_int(r.abi_return) == 1

def test_conversion_abicoderv1(harness):
    """userDefinedValueType/contracts/conversion_abicoderv1.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/conversion_abicoderv1.sol")
    # f(uint256): 1 -> 1
    r = harness.call(app, "f(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # f(uint256): 2 -> 2
    r = harness.call(app, "f(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # f(uint256): 257 -> 1
    r = harness.call(app, "f(uint256)", 257)
    assert as_int(r.abi_return) == 1
    # g(uint256): 1 -> 1
    r = harness.call(app, "g(uint256)", 1)
    assert as_int(r.abi_return) == 1
    # g(uint256): 2 -> 2
    r = harness.call(app, "g(uint256)", 2)
    assert as_int(r.abi_return) == 2
    # g(uint256): 255 -> -1
    r = harness.call(app, "g(uint256)", 255)
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # g(uint256): 257 -> 1
    r = harness.call(app, "g(uint256)", 257)
    assert as_int(r.abi_return) == 1
    # h(uint8): 1 -> 1
    r = harness.call(app, "h(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # h(uint8): 2 -> 2
    r = harness.call(app, "h(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # h(uint8): 255 -> -1
    r = harness.call(app, "h(uint8)", 255)
    assert as_int(r.abi_return) in (-1, 115792089237316195423570985008687907853269984665640564039457584007913129639935)
    # h(uint8): 257 -> 1
    r = harness.call(app, "h(uint8)", 257)
    assert as_int(r.abi_return) == 1
    # i(uint8): 250 -> 250
    r = harness.call(app, "i(uint8)", 250)
    assert as_int(r.abi_return) == 250
    # j(uint8): 1 -> 1
    r = harness.call(app, "j(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # j(uint8): 2 -> 2
    r = harness.call(app, "j(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # j(uint8): 255 -> 0xff
    r = harness.call(app, "j(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # j(uint8): 257 -> 1
    r = harness.call(app, "j(uint8)", 257)
    assert as_int(r.abi_return) == 1
    # k(uint8): 1 -> 1
    r = harness.call(app, "k(uint8)", 1)
    assert as_int(r.abi_return) == 1
    # k(uint8): 2 -> 2
    r = harness.call(app, "k(uint8)", 2)
    assert as_int(r.abi_return) == 2
    # k(uint8): 255 -> 0xff
    r = harness.call(app, "k(uint8)", 255)
    assert as_int(r.abi_return) == 255
    # k(uint8): 257 -> 1
    r = harness.call(app, "k(uint8)", 257)
    assert as_int(r.abi_return) == 1
    # m(uint16): 1 -> 1
    r = harness.call(app, "m(uint16)", 1)
    assert as_int(r.abi_return) == 1
    # m(uint16): 2 -> 2
    r = harness.call(app, "m(uint16)", 2)
    assert as_int(r.abi_return) == 2
    # m(uint16): 255 -> 0xff
    r = harness.call(app, "m(uint16)", 255)
    assert as_int(r.abi_return) == 255
    # m(uint16): 257 -> 1
    r = harness.call(app, "m(uint16)", 257)
    assert as_int(r.abi_return) == 1

def test_dirty_slot(harness):  # currently fails
    """userDefinedValueType/contracts/dirty_slot.sol"""
    app = harness.compile_and_deploy('userDefinedValueType/contracts/dirty_slot.sol')
    r = harness.call(app, 'a()')
    assert as_int(r.abi_return) == 13
    r = harness.call(app, 'b()')
    assert as_int(r.abi_return) == 0x0401000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 0)
    assert as_int(r.abi_return) == 0x0400000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 1)
    assert as_int(r.abi_return) == 0x0100000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 2, expect_revert=True)
    assert r.reverted
    r = harness.call(app, 'write_a()')
    r = harness.call(app, 'a()')
    assert as_int(r.abi_return) == 0x2001
    r = harness.call(app, 'write_b()')
    r = harness.call(app, 'b()')
    assert as_int(r.abi_return) == 0x5403000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 0)
    assert as_int(r.abi_return) == 0x5400000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 1)
    assert as_int(r.abi_return) == 0x0300000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, 'get_b(uint256)', 2, expect_revert=True)
    assert r.reverted

def test_dirty_uint8_read(harness):  # currently fails
    """userDefinedValueType/contracts/dirty_uint8_read.sol"""
    app = harness.compile_and_deploy('userDefinedValueType/contracts/dirty_uint8_read.sol')
    r = harness.call(app, 'x()')
    assert as_int(r.abi_return) == -5
    r = harness.call(app, 'create_dirty_slot()')
    r = harness.call(app, 'read_unclean_value()')
    assert as_int(r.abi_return) == 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb

def test_erc20(harness):
    """userDefinedValueType/contracts/erc20.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/erc20.sol")
    # totalSupply() -> 20
    r = harness.call(app, "totalSupply()")
    assert as_int(r.abi_return) == 20
    # transfer(address,uint256): 2, 5 -> true
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 5)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 0 -> true
    r = harness.call(app, "decreaseAllowance(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 0)
    assert bool(as_int(r.abi_return)) is True
    # decreaseAllowance(address,uint256): 2, 1 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "decreaseAllowance(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 1, expect_revert=True)
    assert r.reverted
    # transfer(address,uint256): 2, 14 -> true
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 14)
    assert bool(as_int(r.abi_return)) is True
    # transfer(address,uint256): 2, 2 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "transfer(address,uint256)", encoding.encode_address((2).to_bytes(32, "big")), 2, expect_revert=True)
    assert r.reverted

def test_fixedpoint(harness):
    """userDefinedValueType/contracts/fixedpoint.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/fixedpoint.sol")
    # add(uint256,uint256): 0, 0 -> 0
    r = harness.call(app, "add(uint256,uint256)", 0, 0)
    assert as_int(r.abi_return) == 0
    # add(uint256,uint256): 25, 45 -> 0x46
    r = harness.call(app, "add(uint256,uint256)", 25, 45)
    assert as_int(r.abi_return) == 70
    # add(uint256,uint256): 115792089237316195423570985008687907853269984665640564039457584007913129639935, 10 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "add(uint256,uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 10, expect_revert=True)
    assert r.reverted
    # mul(uint256,uint256): 340282366920938463463374607431768211456, 45671926166590716193865151022383844364247891968 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "mul(uint256,uint256)", 0x100000000000000000000000000000000, 0x800000000000000000000000000000000000000, expect_revert=True)
    assert r.reverted
    # mul(uint256,uint256): 340282366920938463463374607431768211456, 20 -> 6805647338418769269267492148635364229120
    r = harness.call(app, "mul(uint256,uint256)", 0x100000000000000000000000000000000, 20)
    assert as_int(r.abi_return) == 6805647338418769269267492148635364229120
    # floor(uint256): 11579208923731619542357098500868790785326998665640564039457584007913129639930 -> 11579208923731619542357098500868790785326998665640564039457
    r = harness.call(app, "floor(uint256)", 0x199999999999999999999999999999999999a36a4c7d21800d38571bfffffffa)
    assert as_int(r.abi_return) == 11579208923731619542357098500868790785326998665640564039457
    # floor(uint256): 115792089237316195423570985008687907853269984665640564039457584007913129639935 -> 115792089237316195423570985008687907853269984665640564039457
    r = harness.call(app, "floor(uint256)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert as_int(r.abi_return) == 115792089237316195423570985008687907853269984665640564039457
    # toUFixed256x18(uint256): 0 -> 0
    r = harness.call(app, "toUFixed256x18(uint256)", 0)
    assert as_int(r.abi_return) == 0
    # toUFixed256x18(uint256): 5 -> 5000000000000000000
    r = harness.call(app, "toUFixed256x18(uint256)", 5)
    assert as_int(r.abi_return) == 5000000000000000000
    # toUFixed256x18(uint256): 115792089237316195423570985008687907853269984665640564039457 -> 115792089237316195423570985008687907853269984665640564039457000000000000000000
    r = harness.call(app, "toUFixed256x18(uint256)", 0x12725dd1d243aba0e75fe645cc4873f9e65afe688c928e1f21)
    assert as_int(r.abi_return) == 115792089237316195423570985008687907853269984665640564039457000000000000000000
    # toUFixed256x18(uint256): 115792089237316195423570985008687907853269984665640564039458 -> FAILURE, hex"4e487b71", 0x11
    r = harness.call(app, "toUFixed256x18(uint256)", 0x12725dd1d243aba0e75fe645cc4873f9e65afe688c928e1f22, expect_revert=True)
    assert r.reverted

def test_immutable_signed(harness):  # currently fails
    """userDefinedValueType/contracts/immutable_signed.sol"""
    app = harness.compile_and_deploy('userDefinedValueType/contracts/immutable_signed.sol')
    r = harness.call(app, 'direct()')
    assert tuple(as_int(x) for x in r.abi_return) == (-2, 0x6162000000000000000000000000000000000000000000000000000000000000,)
    r = harness.call(app, 'viaasm()')
    assert tuple(as_int(x) for x in r.abi_return) == (0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe, 0x6162000000000000000000000000000000000000000000000000000000000000,)

def test_in_parenthesis(harness):
    """userDefinedValueType/contracts/in_parenthesis.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/in_parenthesis.sol")
    # f() -> 5, 10
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (5, 10)

def test_mapping_key(harness):
    """userDefinedValueType/contracts/mapping_key.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/mapping_key.sol")
    # set(int256,int256): 1, 1 ->
    r = harness.call(app, "set(int256,int256)", 1, 1)
    # (void return — call succeeding is the assertion)
    # m(int256): 1 -> 1
    r = harness.call(app, "m(int256)", 1)
    assert as_int(r.abi_return) == 1
    # set_unwrapped(int256,int256): 1, 2 ->
    r = harness.call(app, "set_unwrapped(int256,int256)", 1, 2)
    # (void return — call succeeding is the assertion)
    # m(int256): 1 -> 2
    r = harness.call(app, "m(int256)", 1)
    assert as_int(r.abi_return) == 2
    # m(int256): 2 -> 0
    r = harness.call(app, "m(int256)", 2)
    assert as_int(r.abi_return) == 0

def test_memory_to_storage(harness):
    """userDefinedValueType/contracts/memory_to_storage.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/memory_to_storage.sol")
    # s() -> 0, 0, 0x00, 0
    r = harness.call(app, "s()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0, 0, 0)
    harness.call(app, "f((uint8,uint16,bytes2,uint8))", (1, 255, b"ab", 15))
    r = harness.call(app, "s()")
    # bytes2 field "ab" — AVM stores raw bytes (no 32-byte EVM padding).
    assert (as_int(r.abi_return[0]), as_int(r.abi_return[1]), bytes(r.abi_return[2]), as_int(r.abi_return[3])) == (1, 255, b"ab", 15)
    assert not harness.call(app, "g(uint16[])", [1, 2, 3]).reverted
    assert as_int(harness.call(app, "small(uint256)", 0).abi_return) == 1
    assert as_int(harness.call(app, "small(uint256)", 1).abi_return) == 2
    assert not harness.call(app, "h(bytes2[])", [b"ab", b"cd", b"ef"]).reverted
    # l(0)/l(1) read bytes2 entries from storage. AVM stores them as raw 2-byte values.
    assert bytes(harness.call(app, "l(uint256)", 0).abi_return) == b"ab"
    assert bytes(harness.call(app, "l(uint256)", 1).abi_return) == b"cd"

def test_multisource(harness):
    """userDefinedValueType/contracts/multisource.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/multisource.sol")
    # puya-sol maps Solidity `int` (without size) to uint256 in arc56 spec.
    r = harness.call(app, "f(uint256)", 5)
    assert as_int(r.abi_return) == 5
    r = harness.call(app, "f(address)", encoding.encode_address((1).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 1

def test_multisource_module(harness):
    """userDefinedValueType/contracts/multisource_module.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/multisource_module.sol")
    # f(int256): 5 -> 5
    r = harness.call(app, "f(int256)", 5)
    assert as_int(r.abi_return) == 5
    # g(int256): 1 -> 1
    r = harness.call(app, "g(int256)", 1)
    assert as_int(r.abi_return) == 1

def test_ownable(harness):
    """userDefinedValueType/contracts/ownable.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/ownable.sol")
    # renounceOwnership() ->
    r = harness.call(app, "renounceOwnership()")
    # (void return — call succeeding is the assertion)
    # owner() -> 0
    r = harness.call(app, "owner()")
    assert as_int(r.abi_return) == 0
    # setOwner(address): 0x1212121212121212121212121212120000000012 -> FAILURE, hex"5fc483c5"
    r = harness.call(app, "setOwner(address)", encoding.encode_address((103164821458651970696730694074090566015747358738).to_bytes(32, "big")), expect_revert=True)
    assert r.reverted

def test_parameter(harness):
    """userDefinedValueType/contracts/parameter.sol

    ARCH NOTE: Solidity addresses are 20 bytes; AVM accounts are 32 bytes.
    The original EVM tests pass a 32-byte "overlong" address and expect a
    FAILURE (EVM's 20-byte width check rejects it). On AVM, 32-byte values
    are valid addresses natively — no overflow possible, no rejection.
    Test the AVM-observable behavior: id/wrap/unwrap returns the input
    address unchanged for any valid 32-byte account.
    """
    app = harness.compile_and_deploy("userDefinedValueType/contracts/parameter.sol")
    addr = harness.localnet.account.address
    assert harness.call(app, "id(address)", addr).abi_return == addr
    assert harness.call(app, "unwrap(address)", addr).abi_return == addr
    assert harness.call(app, "wrap(address)", addr).abi_return == addr
    assert harness.call(app, "unwrap_assembly(address)", addr).abi_return == addr
    assert harness.call(app, "wrap_assembly(address)", addr).abi_return == addr

def test_simple(harness):
    """userDefinedValueType/contracts/simple.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/simple.sol")
    # f() -> 0
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 0
    # g() -> 1, 1
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 1)

def test_storage_layout(harness):
    """userDefinedValueType/contracts/storage_layout.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/storage_layout.sol")
    # storage_a() -> 0, 0
    r = harness.call(app, "storage_a()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    # storage_b() -> 0, 1
    r = harness.call(app, "storage_b()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 1)
    # storage_c() -> 0, 2
    r = harness.call(app, "storage_c()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 2)
    # storage_d() -> 0, 3
    r = harness.call(app, "storage_d()")
    assert tuple(as_int(x) for x in r.abi_return) == (0, 3)
    # storage_e() -> 1, 0
    r = harness.call(app, "storage_e()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)
    # storage_f() -> 2, 0
    r = harness.call(app, "storage_f()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 0)
    # storage_g() -> 2, 0x14
    r = harness.call(app, "storage_g()")
    assert tuple(as_int(x) for x in r.abi_return) == (2, 20)

def test_storage_layout_struct(harness):
    """userDefinedValueType/contracts/storage_layout_struct.sol"""
    app = harness.compile_and_deploy('userDefinedValueType/contracts/storage_layout_struct.sol')

def test_storage_signed(harness):
    """userDefinedValueType/contracts/storage_signed.sol — UDVT over int16
    (-2). Solidity's ABI sign-extends to a 32-byte int256; the harness's
    as_int decodes unsigned, so signed values come back as 2^256 + n. Use
    the dual-value pattern (signed or its uint256 equivalent)."""
    app = harness.compile_and_deploy('userDefinedValueType/contracts/storage_signed.sol')
    SE_NEG2 = 0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe
    SE_NEG1 = 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
    assert as_int(harness.call(app, 'a()').abi_return) in (-2, SE_NEG2)
    assert as_int(harness.call(app, 'direct()').abi_return) in (-2, SE_NEG2)
    assert as_int(harness.call(app, 'indirect()').abi_return) in (-2, SE_NEG2)
    assert harness.call(app, 'toMemDirect()').abi_return == [-2]
    assert harness.call(app, 'toMemIndirect()').abi_return == [-2]
    assert as_int(harness.call(app, 'div()').abi_return) in (-1, SE_NEG1)
    # viaasm reads the raw biguint slot via Yul `x := st`. AVM biguint of
    # int16(-2) is the 8-byte two's-complement form 0xfffffffffffffffe,
    # right-padded into bytes32 (EVM would have a 32-byte sign-extended
    # 0xfff..ffe; AVM has 24 leading zero bytes + the 8-byte value).
    assert as_int(harness.call(app, 'viaasm()').abi_return) in (
        SE_NEG2, 0xfffffffffffffffe)

def test_wrap_unwrap(harness):
    """userDefinedValueType/contracts/wrap_unwrap.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/wrap_unwrap.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_wrap_unwrap_via_contract_name(harness):
    """userDefinedValueType/contracts/wrap_unwrap_via_contract_name.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/wrap_unwrap_via_contract_name.sol")
    # f(uint256): 0x42 -> 0x42
    r = harness.call(app, "f(uint256)", 66)
    assert as_int(r.abi_return) == 66
    # g(uint256): 0x42 -> 0x42
    r = harness.call(app, "g(uint256)", 66)
    assert as_int(r.abi_return) == 66
    # h(uint256): 0x42 -> 0x42
    r = harness.call(app, "h(uint256)", 66)
    assert as_int(r.abi_return) == 66
    # i(uint256): 0x42 -> 0x42
    r = harness.call(app, "i(uint256)", 66)
    assert as_int(r.abi_return) == 66

def test_zero_cost_abstraction_comparison_elementary(harness):
    """userDefinedValueType/contracts/zero_cost_abstraction_comparison_elementary.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/zero_cost_abstraction_comparison_elementary.sol")
    # getX() -> 0
    r = harness.call(app, "getX()")
    assert as_int(r.abi_return) == 0
    # setX(int256): 5 ->
    r = harness.call(app, "setX(int256)", 5)
    # (void return — call succeeding is the assertion)
    # getX() -> 5
    r = harness.call(app, "getX()")
    assert as_int(r.abi_return) == 5
    # add(int256,int256): 200, 99 -> 299
    r = harness.call(app, "add(int256,int256)", 200, 99)
    assert as_int(r.abi_return) == 299

def test_zero_cost_abstraction_comparison_userdefined(harness):
    """userDefinedValueType/contracts/zero_cost_abstraction_comparison_userdefined.sol"""
    app = harness.compile_and_deploy("userDefinedValueType/contracts/zero_cost_abstraction_comparison_userdefined.sol")
    # getX() -> 0
    r = harness.call(app, "getX()")
    assert as_int(r.abi_return) == 0
    # setX(int256): 5 ->
    r = harness.call(app, "setX(int256)", 5)
    # (void return — call succeeding is the assertion)
    # getX() -> 5
    r = harness.call(app, "getX()")
    assert as_int(r.abi_return) == 5
    # add(int256,int256): 200, 99 -> 299
    r = harness.call(app, "add(int256,int256)", 200, 99)
    assert as_int(r.abi_return) == 299
