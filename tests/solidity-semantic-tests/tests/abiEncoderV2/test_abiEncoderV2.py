"""Tests for the abiEncoderV2 category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_abi_encode_calldata_slice(harness):
    """abiEncoderV2/contracts/abi_encode_calldata_slice.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encode_calldata_slice.sol")
    # test_bytes() ->
    r = harness.call(app, "test_bytes()")
    # (void return — call succeeding is the assertion)
    # test_uint256() ->
    r = harness.call(app, "test_uint256()")
    # (void return — call succeeding is the assertion)

def test_abi_encode_empty_string_v2(harness):
    """abiEncoderV2/contracts/abi_encode_empty_string_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encode_empty_string_v2.sol")
    # f() -> 0x40, 0xa0, 0x40, 0x20, 0x0, 0x0
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 64, 160, 64, 32, 0, 0
    assert not r.reverted

def test_abi_encode_rational_v2(harness):
    """abiEncoderV2/contracts/abi_encode_rational_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encode_rational_v2.sol")
    # f() returns abi.encode(1, -2): two words, -2 as int256 two's complement.
    expected = (1).to_bytes(32, "big") + ((1 << 256) - 2).to_bytes(32, "big")
    assert bytes(harness.call(app, "f()").abi_return) == expected


def test_abi_encode_v2(harness):
    """abiEncoderV2/contracts/abi_encode_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encode_v2.sol")
    # f0() = abi.encode() — empty bytes.
    assert bytes(harness.call(app, "f0()").abi_return) == b""
    # f1() = abi.encode(1, 2).
    assert bytes(harness.call(app, "f1()").abi_return) == (1).to_bytes(32, "big") + (2).to_bytes(32, "big")
    # f2()/f3() = abi.encode(1, "abc", 2): uint, offset, uint, length, padded data.
    expected_f2 = b"".join(v.to_bytes(32, "big") for v in (1, 0x60, 2, 3)) + b"abc".ljust(32, b"\x00")
    assert bytes(harness.call(app, "f2()").abi_return) == expected_f2
    assert bytes(harness.call(app, "f3()").abi_return) == expected_f2
    # f4() = abi.encode(1, "abc", S{a=7, b=[2,3]}, 2). Head: 1, offset_str,
    # offset_S, 2. Tails: string (len=3 + padded data) then S (a, offset_to_b,
    # length, elems). 11 words = 352 bytes.
    expected_f4 = (
        b"".join(v.to_bytes(32, "big") for v in (1, 0x80, 0xc0, 2, 3))
        + b"abc".ljust(32, b"\x00")
        + b"".join(v.to_bytes(32, "big") for v in (7, 0x40, 2, 2, 3))
    )
    assert bytes(harness.call(app, "f4()").abi_return) == expected_f4

def test_abi_encode_v2_in_function_inherited_in_v1_contract(harness):
    """abiEncoderV2/contracts/abi_encode_v2_in_function_inherited_in_v1_contract.sol"""
    # `test()` deploys a child A and cross-calls into it — pre-allocate
    # opcode budget so the chain fits without runtime opup pooling.
    app = harness.compile_and_deploy(
        "abiEncoderV2/contracts/abi_encode_v2_in_function_inherited_in_v1_contract.sol",
        ensure_budget={"test": 20000},
    )
    r = harness.call(app, "test()", extra_fee=20000)
    assert as_int(r.abi_return) == 77

def test_abi_encode_v2_in_modifier_used_in_v1_contract(harness):
    """abiEncoderV2/contracts/abi_encode_v2_in_modifier_used_in_v1_contract.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encode_v2_in_modifier_used_in_v1_contract.sol")
    # test() -> 5, 10
    r = harness.call(app, "test()")
    assert tuple(as_int(x) for x in r.abi_return) == (5, 10)

def test_abi_encoder_v2_head_overflow_with_static_array_cleanup_bug(harness):
    """abiEncoderV2/contracts/abi_encoder_v2_head_overflow_with_static_array_cleanup_bug.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/abi_encoder_v2_head_overflow_with_static_array_cleanup_bug.sol")
    # f(bool,(bytes,uint256[3]),bytes32[2]): 1, 0x80, "a", "b", 0x80, 11, 12, 13, 4, "abcd" -> 1, 0x80, "a", "b", 0x80, 11, 12, 13, 4, "abcd"
    r = harness.call(app, "f(bool,(bytes,uint256[3]),bytes32[2])", 1, 128, bytes.fromhex('61'), bytes.fromhex('62'), 128, 11, 12, 13, 4, bytes.fromhex('61626364'))
    # TODO: verify expected: 1 | 0x80 | "a" | "b" | 0x80 | 11 | 12 | 13 | 4 | "abcd"
    assert not r.reverted

def test_bool_out_of_bounds(harness):
    """abiEncoderV2/contracts/bool_out_of_bounds.sol

    The dirty-byte cases (passing 0x000000 / 0xffffff for a bool arg)
    aren't reachable through algosdk's ARC4 bool encoder. Only the
    canonical True/False values are asserted here.
    """
    app = harness.compile_and_deploy("abiEncoderV2/contracts/bool_out_of_bounds.sol")
    assert harness.call(app, "f(bool)", True).abi_return is True
    assert harness.call(app, "f(bool)", False).abi_return is False

def test_byte_arrays(harness):
    """abiEncoderV2/contracts/byte_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/byte_arrays.sol")
    # f(a, b, c) returns (a, len(b), b[3], c).
    for sig in ("f(uint256,bytes,uint256)", "f_external(uint256,bytes,uint256)"):
        r = harness.call(app, sig, 6, b"abcdefg", 9)
        assert as_int(r.abi_return[0]) == 6
        assert as_int(r.abi_return[1]) == 7
        assert bytes(r.abi_return[2]) == b"d"
        assert as_int(r.abi_return[3]) == 9

def test_calldata_array(harness):
    """abiEncoderV2/contracts/calldata_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array.sol")
    # f takes uint256[][1] — a length-1 static array of dynamic uint256 arrays.
    for inner in ([], [42], [421, 422, 423, 424, 425, 426, 427, 428]):
        r = harness.call(app, "f(uint256[][1])", [inner])
        assert r.abi_return is True

def test_calldata_array_dynamic(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic.sol")
    # f / g return abi.encode(s) for uint256[]: (offset=32, length=3, elems).
    expected_u = b"".join(v.to_bytes(32, "big") for v in (32, 3, 23, 42, 87))
    assert bytes(harness.call(app, "f(uint256[])", [23, 42, 87]).abi_return) == expected_u
    assert bytes(harness.call(app, "g(uint256[])", [23, 42, 87]).abi_return) == expected_u
    # h / i same for uint8[] — values still encode at 32-byte words.
    assert bytes(harness.call(app, "h(uint8[])", [23, 42, 87]).abi_return) == expected_u
    assert bytes(harness.call(app, "i(uint8[])", [23, 42, 87]).abi_return) == expected_u
    # j / k return abi.encode(s) for bytes: (offset=32, length=3, data right-padded).
    expected_b = (
        (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + bytes.fromhex("123456").ljust(32, b"\x00")
    )
    assert bytes(harness.call(app, "j(bytes)", bytes.fromhex("123456")).abi_return) == expected_b
    expected_b2 = (
        (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + bytes.fromhex("ab33ff").ljust(32, b"\x00")
    )
    assert bytes(harness.call(app, "k(bytes)", bytes.fromhex("ab33ff")).abi_return) == expected_b2

def test_calldata_array_dynamic_index_access(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic_index_access.sol")
    assert not harness.call(app, "f(uint256[])", [42, 23, 87]).reverted
    assert not harness.call(app, "g(uint256[][2],uint256)", [[42, 23, 87], [11, 13, 17, 17]], 0).reverted
    assert not harness.call(app, "g(uint256[][2],uint256)", [[42, 23, 87], [11, 13, 17, 17]], 1).reverted
    assert not harness.call(app, "h(uint8[])", [42, 23, 87]).reverted
    assert not harness.call(app, "i(uint8[][2],uint256)", [[42, 23, 87], [11, 13, 17, 17]], 0).reverted
    assert not harness.call(app, "i(uint8[][2],uint256)", [[42, 23, 87], [11, 13, 17, 17]], 1).reverted
    assert not harness.call(app, "j(bytes)", bytes.fromhex("ab11ff")).reverted
    assert not harness.call(app, "k(bytes[2],uint256)", [bytes.fromhex("ab11ff"), bytes.fromhex("ff791432")], 0).reverted
    assert not harness.call(app, "k(bytes[2],uint256)", [bytes.fromhex("ab11ff"), bytes.fromhex("ff791432")], 1).reverted

def test_calldata_array_dynamic_static_dynamic(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic_static_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic_static_dynamic.sol")
    # g() -> 32, 196, hex"eccb829a", 32, 1, 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "g()")
    # TODO: verify expected: 32 | 196 | hex"eccb829a" | 32 | 1 | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted
    # h() -> 32, 196, hex"eccb829a", 32, 1, 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "h()")
    # TODO: verify expected: 32 | 196 | hex"eccb829a" | 32 | 1 | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted

def test_calldata_array_dynamic_static_in_library(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic_static_in_library.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic_static_in_library.sol")
    # f returns (uint256[], uint256[1]) — round-trips its args through L.g.
    r = harness.call(app, "f(uint256[],uint256[1])", [0xff], [0xffff])
    assert [as_int(x) for x in r.abi_return[0]] == [0xff]
    assert [as_int(x) for x in r.abi_return[1]] == [0xffff]

def test_calldata_array_dynamic_static_short_decode(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic_static_short_decode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic_static_short_decode.sol")
    # f(uint256[][2][]): 0x20, 0x01, 0x20, 0x40, 0x60, 0x00, 0x00 -> 23 # this is the common encoding for x.length == 1 && x[0][0].length == 0 && x[0][1].length == 0 #
    r = harness.call(app, "f(uint256[][2][])", 32, 1, 32, 64, 96, 0, 0)
    # TODO: verify expected: 23 # this is the common encoding for x.length == 1 && x[0][0].length == 0 && x[0][1].length == 0 #
    assert not r.reverted
    # f(uint256[][2][]): 0x20, 0x01, 0x20, 0x00, 0x00 -> 23 # exotic, but still valid encoding #
    r = harness.call(app, "f(uint256[][2][])", 32, 1, 32, 0, 0)
    # TODO: verify expected: 23 # exotic | but still valid encoding #
    assert not r.reverted
    # f(uint256[][2][]): 0x20, 0x01, 0x20, 0x00 -> FAILURE # invalid (too short) encoding, but no failure due to this PR #
    r = harness.call(app, "f(uint256[][2][])", 32, 1, 32, 0, expect_revert=True)
    assert r.reverted

def test_calldata_array_dynamic_static_short_reencode(harness):
    """abiEncoderV2/contracts/calldata_array_dynamic_static_short_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_dynamic_static_short_reencode.sol")
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x40, 0x60, 0x00, 0x00 -> 42
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 64, 96, 0, 0)
    assert as_int(r.abi_return) == 42
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x00, 0x00 -> 42
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 0, 0)
    assert as_int(r.abi_return) == 42
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x00 -> FAILURE
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 0, expect_revert=True)
    assert r.reverted

def test_calldata_array_function_types(harness):
    """abiEncoderV2/contracts/calldata_array_function_types.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_function_types.sol")
    # g(bool): false -> 23, 37, 71
    r = harness.call(app, "g(bool)", False)
    assert tuple(as_int(x) for x in r.abi_return) == (23, 37, 71)
    # g(bool): true -> 23, 37, 71
    r = harness.call(app, "g(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (23, 37, 71)

def test_calldata_array_multi_dynamic(harness):
    """abiEncoderV2/contracts/calldata_array_multi_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_multi_dynamic.sol")
    # f(uint256[][]): 0x20, 2, 0x40, 0xC0, 3, 13, 17, 23, 4, 27, 31, 37, 41 -> 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    r = harness.call(app, "f(uint256[][])", 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41)
    # TODO: verify structural decoding matches expected: 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    assert not r.reverted
    # g(uint256[][]): 0x20, 2, 0x40, 0xC0, 3, 13, 17, 23, 4, 27, 31, 37, 41 -> 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    r = harness.call(app, "g(uint256[][])", 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41)
    # TODO: verify structural decoding matches expected: 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    assert not r.reverted
    # h(uint8[][]): 0x20, 2, 0x40, 0xC0, 3, 13, 17, 23, 4, 27, 31, 37, 41 -> 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    r = harness.call(app, "h(uint8[][])", 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41)
    # TODO: verify structural decoding matches expected: 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    assert not r.reverted
    # i(uint8[][]): 0x20, 2, 0x40, 0xC0, 3, 13, 17, 23, 4, 27, 31, 37, 41 -> 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    r = harness.call(app, "i(uint8[][])", 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41)
    # TODO: verify structural decoding matches expected: 32, 416, 32, 2, 64, 192, 3, 13, 17, 23, 4, 27, 31, 37, 41
    assert not r.reverted
    # j(bytes[]): 0x20, 2, 0x40, 0x63, 3, hex"131723", 4, hex"27313741" -> 32, 256, 32, 2, 64, 128, 3, left(0x131723), 4, left(0x27313741)
    r = harness.call(app, "j(bytes[])", 32, 2, 64, 99, 3, bytes.fromhex('131723'), 4, bytes.fromhex('27313741'))
    # TODO: verify expected: 32 | 256 | 32 | 2 | 64 | 128 | 3 | left(0x131723) | 4 | left(0x27313741)
    assert not r.reverted
    # k(bytes[]): 0x20, 2, 0x40, 0x63, 3, hex"131723", 4, hex"27313741" -> 32, 256, 32, 2, 64, 128, 3, left(0x131723), 4, left(0x27313741)
    r = harness.call(app, "k(bytes[])", 32, 2, 64, 99, 3, bytes.fromhex('131723'), 4, bytes.fromhex('27313741'))
    # TODO: verify expected: 32 | 256 | 32 | 2 | 64 | 128 | 3 | left(0x131723) | 4 | left(0x27313741)
    assert not r.reverted

def test_calldata_array_short(harness):
    """abiEncoderV2/contracts/calldata_array_short.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_short.sol")
    # f(uint256[]): 0x20, 0 ->
    r = harness.call(app, "f(uint256[])", 32, 0)
    # (void return — call succeeding is the assertion)
    # f(uint256[]): 0x20, 1 -> FAILURE, hex"08c379a0", 0x20, 0x2b, "ABI decoding: invalid calldata a", "rray stride"
    r = harness.call(app, "f(uint256[])", 32, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[]): 0x20, 2 -> FAILURE, hex"08c379a0", 0x20, 0x2b, "ABI decoding: invalid calldata a", "rray stride"
    r = harness.call(app, "f(uint256[])", 32, 2, expect_revert=True)
    assert r.reverted

def test_calldata_array_short_no_revert_string(harness):
    """abiEncoderV2/contracts/calldata_array_short_no_revert_string.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_short_no_revert_string.sol")
    # f(uint256[]): 0x20, 0 ->
    r = harness.call(app, "f(uint256[])", 32, 0)
    # (void return — call succeeding is the assertion)
    # f(uint256[]): 0x20, 1 -> FAILURE
    r = harness.call(app, "f(uint256[])", 32, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[]): 0x20, 2 -> FAILURE
    r = harness.call(app, "f(uint256[])", 32, 2, expect_revert=True)
    assert r.reverted

def test_calldata_array_static(harness):
    """abiEncoderV2/contracts/calldata_array_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_static.sol")
    # f / g return abi.encode of a static uint256[3]: each element packed as a 32-byte word.
    expected_u = b"".join(v.to_bytes(32, "big") for v in (23, 42, 87))
    assert bytes(harness.call(app, "f(uint256[3])", [23, 42, 87]).abi_return) == expected_u
    assert bytes(harness.call(app, "g(uint256[3])", [23, 42, 87]).abi_return) == expected_u
    # h / i same shape for uint8[3].
    assert bytes(harness.call(app, "h(uint8[3])", [23, 42, 87]).abi_return) == expected_u
    assert bytes(harness.call(app, "i(uint8[3])", [23, 42, 87]).abi_return) == expected_u
    # Overflow / dirty-byte revert cases use values bigger than uint8 — algosdk
    # rejects at encode time before the contract sees them, so the EVM-style
    # negative test isn't reachable here.

def test_calldata_array_static_dynamic_static(harness):
    """abiEncoderV2/contracts/calldata_array_static_dynamic_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_static_dynamic_static.sol")
    # g() -> 32, 132, hex"15cfcc01", 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "g()")
    # TODO: verify expected: 32 | 132 | hex"15cfcc01" | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted
    # h() -> 32, 132, hex"15cfcc01", 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "h()")
    # TODO: verify expected: 32 | 132 | hex"15cfcc01" | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted
    # i() -> 32, 292, hex"dc0ee233", 32, 64, 160, 1, 0x42, 0x000142, 1, 0x010042, 0x010142, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "i()")
    # TODO: verify expected: 32 | 292 | hex"dc0ee233" | 32 | 64 | 160 | 1 | 0x42 | 0x000142 | 1 | 0x010042 | 0x010142 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted
    # j() -> 32, 292, hex"dc0ee233", 32, 64, 160, 1, 0x42, 0x000142, 1, 0x010042, 0x010142, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "j()")
    # TODO: verify expected: 32 | 292 | hex"dc0ee233" | 32 | 64 | 160 | 1 | 0x42 | 0x000142 | 1 | 0x010042 | 0x010142 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted

def test_calldata_array_static_index_access(harness):
    """abiEncoderV2/contracts/calldata_array_static_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_static_index_access.sol")
    # f(uint256[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "f(uint256[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # g(uint256[3][2],uint256): 23, 42, 87, 123, 142, 187, 0 -> 32, 96, 23, 42, 87
    r = harness.call(app, "g(uint256[3][2],uint256)", 23, 42, 87, 123, 142, 187, 0)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # g(uint256[3][2],uint256): 23, 42, 87, 123, 142, 187, 1 -> 32, 96, 123, 142, 187
    r = harness.call(app, "g(uint256[3][2],uint256)", 23, 42, 87, 123, 142, 187, 1)
    # TODO: verify structural decoding matches expected: 32, 96, 123, 142, 187
    assert not r.reverted
    # h(uint8[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "h(uint8[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # i(uint8[3][2],uint256): 23, 42, 87, 123, 142, 187, 0 -> 32, 96, 23, 42, 87
    r = harness.call(app, "i(uint8[3][2],uint256)", 23, 42, 87, 123, 142, 187, 0)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # i(uint8[3][2],uint256): 23, 42, 87, 123, 142, 187, 1 -> 32, 96, 123, 142, 187
    r = harness.call(app, "i(uint8[3][2],uint256)", 23, 42, 87, 123, 142, 187, 1)
    # TODO: verify structural decoding matches expected: 32, 96, 123, 142, 187
    assert not r.reverted

def test_calldata_array_struct_dynamic(harness):
    """abiEncoderV2/contracts/calldata_array_struct_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_struct_dynamic.sol")
    # f((uint256[])[]): 32, 1, 32, 32, 3, 17, 42, 23 -> 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    r = harness.call(app, "f((uint256[])[])", 32, 1, 32, 32, 3, 17, 42, 23)
    # TODO: verify structural decoding matches expected: 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    assert not r.reverted
    # g((uint256[])[]): 32, 1, 32, 32, 3, 17, 42, 23 -> 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    r = harness.call(app, "g((uint256[])[])", 32, 1, 32, 32, 3, 17, 42, 23)
    # TODO: verify structural decoding matches expected: 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    assert not r.reverted

def test_calldata_array_two_dynamic(harness):
    """abiEncoderV2/contracts/calldata_array_two_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_two_dynamic.sol")
    # f(uint256[],uint256[],bool): 0x60, 0xE0, true, 3, 23, 42, 87, 2, 51, 72 -> 32, 160, 0x20, 3, 23, 42, 87
    r = harness.call(app, "f(uint256[],uint256[],bool)", 96, 224, True, 3, 23, 42, 87, 2, 51, 72)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # f(uint256[],uint256[],bool): 0x60, 0xE0, false, 3, 23, 42, 87, 2, 51, 72 -> 32, 128, 0x20, 2, 51, 72
    r = harness.call(app, "f(uint256[],uint256[],bool)", 96, 224, False, 3, 23, 42, 87, 2, 51, 72)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 2, 51, 72
    assert not r.reverted
    # g(uint256[],uint256[],bool): 0x60, 0xE0, true, 3, 23, 42, 87, 2, 51, 72 -> 32, 160, 0x20, 3, 23, 42, 87
    r = harness.call(app, "g(uint256[],uint256[],bool)", 96, 224, True, 3, 23, 42, 87, 2, 51, 72)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # g(uint256[],uint256[],bool): 0x60, 0xE0, false, 3, 23, 42, 87, 2, 51, 72 -> 32, 128, 0x20, 2, 51, 72
    r = harness.call(app, "g(uint256[],uint256[],bool)", 96, 224, False, 3, 23, 42, 87, 2, 51, 72)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 2, 51, 72
    assert not r.reverted

def test_calldata_array_two_static(harness):
    """abiEncoderV2/contracts/calldata_array_two_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_array_two_static.sol")
    # The function picks one of the two static arrays based on the bool and
    # returns its abi.encode. True → uint256[3] = [23, 42, 87] (3 words),
    # False → uint256[2] = [51, 72] (2 words).
    expected_true = b"".join(v.to_bytes(32, "big") for v in (23, 42, 87))
    expected_false = b"".join(v.to_bytes(32, "big") for v in (51, 72))
    for sig in ("f(uint256[3],uint256[2],bool)", "g(uint256[3],uint256[2],bool)"):
        assert bytes(harness.call(app, sig, [23, 42, 87], [51, 72], True).abi_return) == expected_true
        assert bytes(harness.call(app, sig, [23, 42, 87], [51, 72], False).abi_return) == expected_false

def test_calldata_dynamic_array_to_memory(harness):
    """abiEncoderV2/contracts/calldata_dynamic_array_to_memory.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_dynamic_array_to_memory.sol")
    # f(uint256[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8 -> 0x20, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8
    r = harness.call(app, "f(uint256[][])", 32, 2, 64, 160, 2, 5, 6, 2, 7, 8)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 160, 2, 5, 6, 2, 7, 8
    assert not r.reverted
    # f(uint256[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8, 9 -> 0x20, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8
    r = harness.call(app, "f(uint256[][])", 32, 2, 64, 160, 2, 5, 6, 2, 7, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 160, 2, 5, 6, 2, 7, 8
    assert not r.reverted
    # f(uint256[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8 -> FAILURE
    r = harness.call(app, "f(uint256[][])", 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, expect_revert=True)
    assert r.reverted
    # g(uint256[][][]): 0x20, 2, 0x40, 0x60, 0, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8 -> 0x20, 2, 0x40, 0x60, 0, 2, 0x40, 0xa0, 2, 5, 6, 2, 7, 8
    r = harness.call(app, "g(uint256[][][])", 32, 2, 64, 96, 0, 2, 64, 160, 2, 5, 6, 2, 7, 8)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 96, 0, 2, 64, 160, 2, 5, 6, 2, 7, 8
    assert not r.reverted
    # g(uint256[][][]): 0x20, 2, 0x40, 0x60, 0, 2, 0x40, 0xa0, 2, 5, 6, 2, 7 -> FAILURE
    r = harness.call(app, "g(uint256[][][])", 32, 2, 64, 96, 0, 2, 64, 160, 2, 5, 6, 2, 7, expect_revert=True)
    assert r.reverted
    # h(uint256[2][][]): 0x20, 2, 0x40, 0x60, 0, 2, 5, 6, 7, 8 -> 0x20, 2, 0x40, 0x60, 0, 2, 5, 6, 7, 8
    r = harness.call(app, "h(uint256[2][][])", 32, 2, 64, 96, 0, 2, 5, 6, 7, 8)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 96, 0, 2, 5, 6, 7, 8
    assert not r.reverted
    # h(uint256[2][][]): 0x20, 2, 0x40, 0x60, 0, 2, 5, 6, 7, 8, 9 -> 0x20, 2, 0x40, 0x60, 0, 2, 5, 6, 7, 8
    r = harness.call(app, "h(uint256[2][][])", 32, 2, 64, 96, 0, 2, 5, 6, 7, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 96, 0, 2, 5, 6, 7, 8
    assert not r.reverted
    # h(uint256[2][][]): 0x20, 2, 0x40, 0x60, 0, 2, 5, 6, 7 -> FAILURE
    r = harness.call(app, "h(uint256[2][][])", 32, 2, 64, 96, 0, 2, 5, 6, 7, expect_revert=True)
    assert r.reverted

def test_calldata_nested_array_reencode(harness):
    """abiEncoderV2/contracts/calldata_nested_array_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_nested_array_reencode.sol")
    # f(uint256[][]): 0x20, 1, 0x20, 0 -> 0x20, 0x80, 0x20, 1, 0x20, 0
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 0)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 1, 32, 0
    assert not r.reverted
    # f(uint256[][]): 0x20, 1, 0x20, 1 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[][]): 0x20, 1, 0x20, 2 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 2, expect_revert=True)
    assert r.reverted
    # f(uint256[][]): 0x20, 1, 0x20, 3 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "f(uint256[][])", 32, 1, 32, 3, expect_revert=True)
    assert r.reverted
    # g(uint8[][][]): 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 0 -> 0x20, 0x01a0, 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 0
    r = harness.call(app, "g(uint8[][][])", 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 0)
    # TODO: verify structural decoding matches expected: 32, 416, 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 0
    assert not r.reverted
    # g(uint8[][][]): 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access offset"
    r = harness.call(app, "g(uint8[][][])", 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, expect_revert=True)
    assert r.reverted
    # g(uint8[][][]): 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 1, 0x20, 0 -> 0x20, 0x01e0, 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 1, 0x20, 0
    r = harness.call(app, "g(uint8[][][])", 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 1, 32, 0)
    # TODO: verify structural decoding matches expected: 32, 480, 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 1, 32, 0
    assert not r.reverted
    # g(uint8[][][]): 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 1, 0x20, 0, 1 -> 0x20, 0x01e0, 0x20, 2, 0x40, 0x0140, 2, 0x40, 0x80, 1, 10, 2, 11, 12, 1, 0x20, 0
    r = harness.call(app, "g(uint8[][][])", 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 1, 32, 0, 1)
    # TODO: verify structural decoding matches expected: 32, 480, 32, 2, 64, 320, 2, 64, 128, 1, 10, 2, 11, 12, 1, 32, 0
    assert not r.reverted
    # h(uint16[][2][]): 0x20, 2, 0x40, 0x0120, 0x40, 0x80, 1, 10, 2, 11, 12, 0x40, 0x60, 0, 1, 13 -> 0x20, 0x0200, 0x20, 2, 0x40, 288, 0x40, 0x80, 1, 10, 2, 11, 12, 0x40, 0x60, 0, 1, 13
    r = harness.call(app, "h(uint16[][2][])", 32, 2, 64, 288, 64, 128, 1, 10, 2, 11, 12, 64, 96, 0, 1, 13)
    # TODO: verify structural decoding matches expected: 32, 512, 32, 2, 64, 288, 64, 128, 1, 10, 2, 11, 12, 64, 96, 0, 1, 13
    assert not r.reverted
    # h(uint16[][2][]): 0x20, 2, 0x40, 0x0120, 0x40, 0x80, 1, 10, 2, 11, 12, 0x40, 0x60, 0, 1 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "h(uint16[][2][])", 32, 2, 64, 288, 64, 128, 1, 10, 2, 11, 12, 64, 96, 0, 1, expect_revert=True)
    assert r.reverted
    # i(uint16[][][1]): 0x20, 0x20, 2, 0x40, 0x80, 1, 10, 2, 11, 12 -> 0x20, 0x0140, 0x20, 0x20, 2, 0x40, 0x80, 1, 10, 2, 11, 12
    r = harness.call(app, "i(uint16[][][1])", 32, 32, 2, 64, 128, 1, 10, 2, 11, 12)
    # TODO: verify structural decoding matches expected: 32, 320, 32, 32, 2, 64, 128, 1, 10, 2, 11, 12
    assert not r.reverted
    # i(uint16[][][1]): 0x20, 0x20, 2, 0x40, 0x80, 1, 10, 2, 11 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "i(uint16[][][1])", 32, 32, 2, 64, 128, 1, 10, 2, 11, expect_revert=True)
    assert r.reverted
    # j(uint16[2][][]): 0x20, 2, 0x40, 0xa0, 1, 0x0a, 11, 2, 12, 13, 14, 15 -> 0x20, 0x0180, 0x20, 2, 0x40, 0xa0, 1, 10, 11, 2, 12, 13, 14, 15
    r = harness.call(app, "j(uint16[2][][])", 32, 2, 64, 160, 1, 10, 11, 2, 12, 13, 14, 15)
    # TODO: verify structural decoding matches expected: 32, 384, 32, 2, 64, 160, 1, 10, 11, 2, 12, 13, 14, 15
    assert not r.reverted
    # j(uint16[2][][]): 0x20, 2, 0x40, 0xa0, 1, 0x0a, 11, 2, 12, 13, 14 -> FAILURE, hex"08c379a0", 0x20, 0x1e, "Invalid calldata access stride"
    r = harness.call(app, "j(uint16[2][][])", 32, 2, 64, 160, 1, 10, 11, 2, 12, 13, 14, expect_revert=True)
    assert r.reverted

def test_calldata_nested_array_static_reencode(harness):
    """abiEncoderV2/contracts/calldata_nested_array_static_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_nested_array_static_reencode.sol")
    # f(uint256[3][]): 0x20, 1, 0x01 -> FAILURE
    r = harness.call(app, "f(uint256[3][])", 32, 1, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[3][]): 0x20, 1, 0x01, 0x02 -> FAILURE
    r = harness.call(app, "f(uint256[3][])", 32, 1, 1, 2, expect_revert=True)
    assert r.reverted
    # f(uint256[3][]): 0x20, 1, 0x01, 0x02, 0x03 ->
    r = harness.call(app, "f(uint256[3][])", 32, 1, 1, 2, 3)
    # (void return — call succeeding is the assertion)
    # f(uint256[][3]): 0x20, 0x60, 0x60, 0x60, 3, 0x01 -> FAILURE
    r = harness.call(app, "f(uint256[][3])", 32, 96, 96, 96, 3, 1, expect_revert=True)
    assert r.reverted
    # f(uint256[][3]): 0x20, 0x60, 0x60, 0x60, 3, 0x01, 0x02 -> FAILURE
    r = harness.call(app, "f(uint256[][3])", 32, 96, 96, 96, 3, 1, 2, expect_revert=True)
    assert r.reverted
    # f(uint256[][3]): 0x20, 0x60, 0x60, 0x60, 3, 0x01, 0x02, 0x03 ->
    r = harness.call(app, "f(uint256[][3])", 32, 96, 96, 96, 3, 1, 2, 3)
    # (void return — call succeeding is the assertion)
    # f(uint256[2][2]): 0x01 -> FAILURE
    r = harness.call(app, "f(uint256[2][2])", 1, expect_revert=True)
    assert r.reverted
    # f(uint256[2][2]): 0x01, 0x02 -> FAILURE
    r = harness.call(app, "f(uint256[2][2])", 1, 2, expect_revert=True)
    assert r.reverted
    # f(uint256[2][2]): 0x01, 0x02, 0x03 -> FAILURE
    r = harness.call(app, "f(uint256[2][2])", 1, 2, 3, expect_revert=True)
    assert r.reverted
    # f(uint256[2][2]): 0x01, 0x02, 0x03, 0x04 ->
    r = harness.call(app, "f(uint256[2][2])", 1, 2, 3, 4)
    # (void return — call succeeding is the assertion)
    # f(uint256[2][2]): 0x01, 0x02, 0x03, 0x04, 0x05 ->
    r = harness.call(app, "f(uint256[2][2])", 1, 2, 3, 4, 5)
    # (void return — call succeeding is the assertion)

def test_calldata_overlapped_dynamic_arrays(harness):
    """abiEncoderV2/contracts/calldata_overlapped_dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_overlapped_dynamic_arrays.sol")
    # f_memory(uint256[],uint256[2]): 0x20, 1, 2 -> 0x60, 0x01, 0x02, 1, 2
    r = harness.call(app, "f_memory(uint256[],uint256[2])", 32, 1, 2)
    # TODO: verify structural decoding matches expected: 96, 1, 2, 1, 2
    assert not r.reverted
    # f_memory(uint256[],uint256[2]): 0x40, 1, 2, 5, 6 -> 0x60, 1, 2, 2, 5, 6
    r = harness.call(app, "f_memory(uint256[],uint256[2])", 64, 1, 2, 5, 6)
    # TODO: verify structural decoding matches expected: 96, 1, 2, 2, 5, 6
    assert not r.reverted
    # f_memory(uint256[],uint256[2]): 0x40, 1, 2, 5 -> FAILURE
    r = harness.call(app, "f_memory(uint256[],uint256[2])", 64, 1, 2, 5, expect_revert=True)
    assert r.reverted
    # f_encode(uint256[],uint256[2]): 0x20, 1, 2 -> 0x20, 0xa0, 0x60, 1, 2, 1, 2
    r = harness.call(app, "f_encode(uint256[],uint256[2])", 32, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 160, 96, 1, 2, 1, 2
    assert not r.reverted
    # f_encode(uint256[],uint256[2]): 0x40, 1, 2, 5, 6 -> 0x20, 0xc0, 0x60, 1, 2, 2, 5, 6
    r = harness.call(app, "f_encode(uint256[],uint256[2])", 64, 1, 2, 5, 6)
    # TODO: verify structural decoding matches expected: 32, 192, 96, 1, 2, 2, 5, 6
    assert not r.reverted
    # f_encode(uint256[],uint256[2]): 0x40, 1, 2, 5 -> FAILURE
    r = harness.call(app, "f_encode(uint256[],uint256[2])", 64, 1, 2, 5, expect_revert=True)
    assert r.reverted
    # f_which(uint256[],uint256[2],uint256): 0x40, 1, 2, 1, 5 -> 0x20, 0x40, 5, 2
    r = harness.call(app, "f_which(uint256[],uint256[2],uint256)", 64, 1, 2, 1, 5)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 5, 2)
    # f_which(uint256[],uint256[2],uint256): 0x40, 1, 2, 1, 5, 6 -> 0x20, 0x40, 5, 2
    r = harness.call(app, "f_which(uint256[],uint256[2],uint256)", 64, 1, 2, 1, 5, 6)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 5, 2)
    # f_which(uint256[],uint256[2],uint256): 0x40, 1, 2, 1 -> FAILURE
    r = harness.call(app, "f_which(uint256[],uint256[2],uint256)", 64, 1, 2, 1, expect_revert=True)
    assert r.reverted
    # f_storage(uint256[],uint256[2]): 0x20, 1, 2 -> 0x20, 0x60, 0x20, 1, 2
    r = harness.call(app, "f_storage(uint256[],uint256[2])", 32, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 2
    assert not r.reverted
    # f_storage(uint256[],uint256[2]): 0x40, 1, 2, 5, 6 -> 0x20, 0x80, 0x20, 2, 5, 6
    r = harness.call(app, "f_storage(uint256[],uint256[2])", 64, 1, 2, 5, 6)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 2, 5, 6
    assert not r.reverted
    # f_storage(uint256[],uint256[2]): 0x40, 1, 2, 5 -> FAILURE
    r = harness.call(app, "f_storage(uint256[],uint256[2])", 64, 1, 2, 5, expect_revert=True)
    assert r.reverted

def test_calldata_overlapped_nested_dynamic_arrays(harness):
    """abiEncoderV2/contracts/calldata_overlapped_nested_dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_overlapped_nested_dynamic_arrays.sol")
    # f_memory(uint256[][]): 0x20, 2, 0x40, 0x40, 2, 1, 2 -> 0x20, 2, 0x40, 0xa0, 2, 1, 2, 2, 1, 2
    r = harness.call(app, "f_memory(uint256[][])", 32, 2, 64, 64, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 160, 2, 1, 2, 2, 1, 2
    assert not r.reverted
    # f_memory(uint256[][]): 0x20, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 2, 0x40, 0xa0, 2, 1, 2, 1, 2
    r = harness.call(app, "f_memory(uint256[][])", 32, 2, 64, 96, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 160, 2, 1, 2, 1, 2
    assert not r.reverted
    # f_memory(uint256[][]): 0x20, 2, 0, 0x60, 2, 1, 2 -> 0x20, 2, 0x40, 0x60, 0, 1, 2
    r = harness.call(app, "f_memory(uint256[][])", 32, 2, 0, 96, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 2, 64, 96, 0, 1, 2
    assert not r.reverted
    # f_memory(uint256[][]): 0x20, 2, 0, 0x60, 2, 2, 2 -> FAILURE
    r = harness.call(app, "f_memory(uint256[][])", 32, 2, 0, 96, 2, 2, 2, expect_revert=True)
    assert r.reverted
    # f_encode(uint256[][]): 0x20, 2, 0x40, 0x40, 2, 1, 2 -> 0x20, 0x0140, 0x20, 2, 0x40, 0xa0, 2, 1, 2, 2, 1, 2
    r = harness.call(app, "f_encode(uint256[][])", 32, 2, 64, 64, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 320, 32, 2, 64, 160, 2, 1, 2, 2, 1, 2
    assert not r.reverted
    # f_encode(uint256[][]): 0x20, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 0x0120, 0x20, 2, 0x40, 0xa0, 2, 1, 2, 1, 2
    r = harness.call(app, "f_encode(uint256[][])", 32, 2, 64, 96, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 288, 32, 2, 64, 160, 2, 1, 2, 1, 2
    assert not r.reverted
    # f_encode(uint256[][]): 0x20, 2, 0, 0x60, 2, 1, 2 -> 0x20, 0xe0, 0x20, 2, 0x40, 0x60, 0, 1, 2
    r = harness.call(app, "f_encode(uint256[][])", 32, 2, 0, 96, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 224, 32, 2, 64, 96, 0, 1, 2
    assert not r.reverted
    # f_encode(uint256[][]): 0x20, 2, 0, 0x60, 2, 2, 2 -> FAILURE
    r = harness.call(app, "f_encode(uint256[][])", 32, 2, 0, 96, 2, 2, 2, expect_revert=True)
    assert r.reverted
    # f_which(uint256[][],uint256): 0x40, 0, 2, 0x40, 0x40, 2, 1, 2 -> 0x20, 2, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 0, 2, 64, 64, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0x40, 0x40, 2, 1, 2 -> 0x20, 2, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 64, 64, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 0, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 2, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 0, 2, 64, 96, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 64, 96, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 0, 2, 0, 0x60, 2, 1, 2 -> 0x20, 0
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 0, 2, 0, 96, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0, 0x60, 2, 1, 2 -> 0x20, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 0, 96, 2, 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0, 0x60, 2, 2, 2 -> FAILURE
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 0, 96, 2, 2, 2, expect_revert=True)
    assert r.reverted

def test_calldata_struct_array_reencode(harness):
    """abiEncoderV2/contracts/calldata_struct_array_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_struct_array_reencode.sol")
    # f((uint256[])): 0x20, 0x20, 0 -> 0x20, 0x60, 0x20, 0x20, 0
    r = harness.call(app, "f((uint256[]))", 32, 32, 0)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 32, 0
    assert not r.reverted
    # f((uint256[])): 0x20, 0x20, 1 -> FAILURE
    r = harness.call(app, "f((uint256[]))", 32, 32, 1, expect_revert=True)
    assert r.reverted
    # f((uint256[])): 0x20, 0x20, 2 -> FAILURE
    r = harness.call(app, "f((uint256[]))", 32, 32, 2, expect_revert=True)
    assert r.reverted
    # f((uint256[])): 0x20, 0x20, 3 -> FAILURE
    r = harness.call(app, "f((uint256[]))", 32, 32, 3, expect_revert=True)
    assert r.reverted
    # g((uint256[])[2]): 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3 -> 0x20, 0x0140, 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3
    r = harness.call(app, "g((uint256[])[2])", 32, 64, 192, 32, 2, 1, 2, 32, 1, 3)
    # TODO: verify structural decoding matches expected: 32, 320, 32, 64, 192, 32, 2, 1, 2, 32, 1, 3
    assert not r.reverted
    # g((uint256[])[2]): 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1 -> FAILURE
    r = harness.call(app, "g((uint256[])[2])", 32, 64, 192, 32, 2, 1, 2, 32, 1, expect_revert=True)
    assert r.reverted
    # h((uint256[])[][]): 0x20, 0x02, 0x40, 0x0180, 2, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3, 1, 0x20, 0x20, 1, 1 -> 0x20, 0x0260, 0x20, 2, 0x40, 0x0180, 2, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3, 1, 0x20, 0x20, 1, 1
    r = harness.call(app, "h((uint256[])[][])", 32, 2, 64, 384, 2, 64, 192, 32, 2, 1, 2, 32, 1, 3, 1, 32, 32, 1, 1)
    # TODO: verify structural decoding matches expected: 32, 608, 32, 2, 64, 384, 2, 64, 192, 32, 2, 1, 2, 32, 1, 3, 1, 32, 32, 1, 1
    assert not r.reverted
    # h((uint256[])[][]): 0x20, 0x02, 0x40, 0x0180, 2, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3, 1, 0x20, 0x20, 1 -> FAILURE
    r = harness.call(app, "h((uint256[])[][])", 32, 2, 64, 384, 2, 64, 192, 32, 2, 1, 2, 32, 1, 3, 1, 32, 32, 1, expect_revert=True)
    assert r.reverted
    # i((uint256[])[2][]): 0x20, 1, 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3 -> 0x20, 0x0180, 0x20, 1, 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1, 3
    r = harness.call(app, "i((uint256[])[2][])", 32, 1, 32, 64, 192, 32, 2, 1, 2, 32, 1, 3)
    # TODO: verify structural decoding matches expected: 32, 384, 32, 1, 32, 64, 192, 32, 2, 1, 2, 32, 1, 3
    assert not r.reverted
    # i((uint256[])[2][]): 0x20, 1, 0x20, 0x40, 0xc0, 0x20, 2, 1, 2, 0x20, 1 -> FAILURE
    r = harness.call(app, "i((uint256[])[2][])", 32, 1, 32, 64, 192, 32, 2, 1, 2, 32, 1, expect_revert=True)
    assert r.reverted
    # j((uint256)[]): 0x20, 2, 1, 2 -> 0x20, 0x80, 0x20, 2, 1, 2
    r = harness.call(app, "j((uint256)[])", 32, 2, 1, 2)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 2, 1, 2
    assert not r.reverted
    # j((uint256)[]): 0x20, 2, 1 -> FAILURE
    r = harness.call(app, "j((uint256)[])", 32, 2, 1, expect_revert=True)
    assert r.reverted
    # k((uint256)[2]): 1, 2 -> 0x20, 0x40, 1, 2
    r = harness.call(app, "k((uint256)[2])", 1, 2)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 1, 2)
    # k((uint256)[2]): 1 -> FAILURE
    r = harness.call(app, "k((uint256)[2])", 1, expect_revert=True)
    assert r.reverted
    # l((uint256)[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8, 9 -> 0x20, 0x0160, 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8, 9
    r = harness.call(app, "l((uint256)[][])", 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 352, 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9
    assert not r.reverted
    # l((uint256)[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8, 9, 10 -> 0x20, 0x0160, 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8, 9
    r = harness.call(app, "l((uint256)[][])", 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9, 10)
    # TODO: verify structural decoding matches expected: 32, 352, 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9
    assert not r.reverted
    # l((uint256)[][]): 0x20, 2, 0x40, 0xa0, 2, 5, 6, 3, 7, 8 -> FAILURE
    r = harness.call(app, "l((uint256)[][])", 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, expect_revert=True)
    assert r.reverted

def test_calldata_struct_dynamic(harness):
    """abiEncoderV2/contracts/calldata_struct_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_struct_dynamic.sol")
    # f((uint256[])): 0x20, 0x20, 3, 42, 23, 17 -> 32, 192, 0x20, 0x20, 3, 42, 23, 17
    r = harness.call(app, "f((uint256[]))", 32, 32, 3, 42, 23, 17)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 32, 3, 42, 23, 17
    assert not r.reverted
    # g((uint256[])): 0x20, 0x20, 3, 42, 23, 17 -> 32, 192, 0x20, 0x20, 3, 42, 23, 17
    r = harness.call(app, "g((uint256[]))", 32, 32, 3, 42, 23, 17)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 32, 3, 42, 23, 17
    assert not r.reverted

def test_calldata_struct_member_offset(harness):
    """abiEncoderV2/contracts/calldata_struct_member_offset.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_struct_member_offset.sol")
    # f() -> 11, 11
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (11, 11)

def test_calldata_struct_simple(harness):
    """abiEncoderV2/contracts/calldata_struct_simple.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_struct_simple.sol")
    # f / g return abi.encode of a struct {uint a} = one 32-byte word.
    expected = (3).to_bytes(32, "big")
    assert bytes(harness.call(app, "f((uint256))", (3,)).abi_return) == expected
    assert bytes(harness.call(app, "g((uint256))", (3,)).abi_return) == expected

def test_calldata_three_dimensional_dynamic_array_index_access(harness):
    """abiEncoderV2/contracts/calldata_three_dimensional_dynamic_array_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_three_dimensional_dynamic_array_index_access.sol")
    # f(uint256[][],uint256,uint256): 0x60, 0, 0, 2, 0x40, 0x80, 1, 7, 1, 8 -> 0x20, 0x20, 7
    r = harness.call(app, "f(uint256[][],uint256,uint256)", 96, 0, 0, 2, 64, 128, 1, 7, 1, 8)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 7)
    # f(uint256[][],uint256,uint256): 0x60, 1, 0, 2, 0x40, 0x80, 1, 7, 1, 8 -> 0x20, 0x20, 8
    r = harness.call(app, "f(uint256[][],uint256,uint256)", 96, 1, 0, 2, 64, 128, 1, 7, 1, 8)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 8)
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 0, 0, 0, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> 0x20, 0x20, 4
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 0, 0, 0, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 4)
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 1, 0, 1, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> 0x20, 0x20, 6
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 1, 0, 1, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 32, 6)
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 1, 0, 2, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 1, 0, 2, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7, expect_revert=True)
    assert r.reverted
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 2, 0, 1, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 2, 0, 1, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7, expect_revert=True)
    assert r.reverted
    # h(uint256[][][1],uint256): 0x40, 1, 0x20, 2, 0x40, 0xA0, 2, 5, 6, 3, 7, 8, 9 -> 0x20, 0xa0, 0x20, 3, 7, 8, 9
    r = harness.call(app, "h(uint256[][][1],uint256)", 64, 1, 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 7, 8, 9
    assert not r.reverted
    # h(uint256[][][1],uint256): 0x40, 2, 0x20, 2, 0x40, 0xA0, 2, 5, 6, 3, 7, 8, 9 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "h(uint256[][][1],uint256)", 64, 2, 32, 2, 64, 160, 2, 5, 6, 3, 7, 8, 9, expect_revert=True)
    assert r.reverted
    # k((uint256[])[][],uint256,uint256): 0x60, 0, 0, 2, 0x40, 0xe0, 1, 0x20, 0x20, 1, 6, 2, 0x40, 0xa0, 0x20, 1, 7, 0x20, 2, 8, 9 -> 0x20, 0x60, 0x20, 1, 6
    r = harness.call(app, "k((uint256[])[][],uint256,uint256)", 96, 0, 0, 2, 64, 224, 1, 32, 32, 1, 6, 2, 64, 160, 32, 1, 7, 32, 2, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 6
    assert not r.reverted
    # k((uint256[])[][],uint256,uint256): 0x60, 0, 1, 2, 0x40, 0xe0, 1, 0x20, 0x20, 1, 6, 2, 0x40, 0xa0, 0x20, 1, 7, 0x20, 2, 8, 9 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "k((uint256[])[][],uint256,uint256)", 96, 0, 1, 2, 64, 224, 1, 32, 32, 1, 6, 2, 64, 160, 32, 1, 7, 32, 2, 8, 9, expect_revert=True)
    assert r.reverted
    # l((uint256[])[2][2],uint256,uint256): 0x60, 1, 1, 0x40, 0x0140, 0x40, 0xa0, 0x20, 1, 5, 0x20, 1, 6, 0x40, 0xa0, 0x20, 1, 7, 0x20, 2, 8, 9 -> 0x20, 0x80, 0x20, 2, 8, 9
    r = harness.call(app, "l((uint256[])[2][2],uint256,uint256)", 96, 1, 1, 64, 320, 64, 160, 32, 1, 5, 32, 1, 6, 64, 160, 32, 1, 7, 32, 2, 8, 9)
    # TODO: verify structural decoding matches expected: 32, 128, 32, 2, 8, 9
    assert not r.reverted
    # l((uint256[])[2][2],uint256,uint256): 0x60, 1, 2, 0x40, 0x0140, 0x40, 0xa0, 0x20, 1, 5, 0x20, 1, 6, 0x40, 0xa0, 0x20, 1, 7, 0x20, 2, 8, 9 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "l((uint256[])[2][2],uint256,uint256)", 96, 1, 2, 64, 320, 64, 160, 32, 1, 5, 32, 1, 6, 64, 160, 32, 1, 7, 32, 2, 8, 9, expect_revert=True)
    assert r.reverted

def test_calldata_with_garbage(harness):
    """abiEncoderV2/contracts/calldata_with_garbage.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/calldata_with_garbage.sol")
    # f_memory(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # f_memory(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 1, 7
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 1, 7)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 1, 7)
    # f_memory(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_encode(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # f_encode(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 0x60, 0x20, 1, 7
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 7
    assert not r.reverted
    # f_encode(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_storage(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (32, 64, 32, 0)
    # f_storage(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 0x60, 0x20, 1, 7
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 7
    assert not r.reverted
    # f_storage(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_index(uint256[],uint256): 0xa0, 0, 9, 9, 9, 2, 7, 8 -> 7
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 0, 9, 9, 9, 2, 7, 8)
    assert as_int(r.abi_return) == 7
    # f_index(uint256[],uint256): 0xa0, 1, 9, 9, 9, 2, 7, 8 -> 8
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 1, 9, 9, 9, 2, 7, 8)
    assert as_int(r.abi_return) == 8
    # f_index(uint256[],uint256): 0xa0, 2, 9, 9, 9, 2, 7, 8 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 2, 9, 9, 9, 2, 7, 8, expect_revert=True)
    assert r.reverted
    # g_memory(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 0 -> 0x60, 1, 2, 0
    r = harness.call(app, "g_memory(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 0)
    assert tuple(as_int(x) for x in r.abi_return) == (96, 1, 2, 0)
    # g_memory(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 1, 7 -> 0x60, 1, 2, 1, 7
    r = harness.call(app, "g_memory(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 96, 1, 2, 1, 7
    assert not r.reverted
    # g_memory(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "g_memory(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # g_encode(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 0 -> 0x20, 0x80, 0x60, 1, 2, 0
    r = harness.call(app, "g_encode(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 0)
    # TODO: verify structural decoding matches expected: 32, 128, 96, 1, 2, 0
    assert not r.reverted
    # g_encode(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 1, 7 -> 0x20, 0xa0, 0x60, 1, 2, 1, 7
    r = harness.call(app, "g_encode(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 160, 96, 1, 2, 1, 7
    assert not r.reverted
    # g_encode(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "g_encode(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # g_storage(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 0 -> 0x20, 0x80, 0x60, 1, 2, 0
    r = harness.call(app, "g_storage(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 0)
    # TODO: verify structural decoding matches expected: 32, 128, 96, 1, 2, 0
    assert not r.reverted
    # g_storage(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 1, 7 -> 0x20, 0xa0, 0x60, 1, 2, 1, 7
    r = harness.call(app, "g_storage(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 160, 96, 1, 2, 1, 7
    assert not r.reverted
    # g_storage(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "g_storage(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # g_index(uint256[],uint256[2],uint256): 0xe0, 1, 2, 0, 9, 9, 9, 2, 7, 8 -> 7, 1
    r = harness.call(app, "g_index(uint256[],uint256[2],uint256)", 224, 1, 2, 0, 9, 9, 9, 2, 7, 8)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 1)
    # g_index(uint256[],uint256[2],uint256): 0xe0, 1, 2, 1, 9, 9, 9, 2, 7, 8 -> 8, 1
    r = harness.call(app, "g_index(uint256[],uint256[2],uint256)", 224, 1, 2, 1, 9, 9, 9, 2, 7, 8)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 1)
    # g_index(uint256[],uint256[2],uint256): 0xe0, 1, 2, 1, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "g_index(uint256[],uint256[2],uint256)", 224, 1, 2, 1, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted

def test_dynamic_arrays(harness):
    """abiEncoderV2/contracts/dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/dynamic_arrays.sol")
    # f(a, b[], c) returns (len(b), b[a], c). With a=6, b=[11..17], c=9.
    r = harness.call(app, "f(uint256,uint16[],uint256)", 6, [11, 12, 13, 14, 15, 16, 17], 9)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 17, 9)

def test_dynamic_nested_arrays(harness):
    """abiEncoderV2/contracts/dynamic_nested_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/dynamic_nested_arrays.sol")
    # test() -> 12, 3, 4, 0x66, 5, 0x85, 13
    r = harness.call(app, "test()")
    # TODO: verify structural decoding matches expected: 12, 3, 4, 102, 5, 133, 13
    assert not r.reverted
    # f(uint256,uint16[][],uint256[2][][3],uint256): 12, 0x80, 0x220, 13, 3, 0x60, 0xC0, 0x160, 2, 85, 86, 4, 101, 102, 103, 104, 0, 0x60, 0xC0, 0x220, 1, 0, 117, 5, 0, 0, 0, 133, 0, 0, 0, 0, 0, 0, 0 -> 12, 3, 4, 0x66, 5, 0x85, 13
    r = harness.call(app, "f(uint256,uint16[][],uint256[2][][3],uint256)", 12, 128, 544, 13, 3, 96, 192, 352, 2, 85, 86, 4, 101, 102, 103, 104, 0, 96, 192, 544, 1, 0, 117, 5, 0, 0, 0, 133, 0, 0, 0, 0, 0, 0, 0)
    # TODO: verify structural decoding matches expected: 12, 3, 4, 102, 5, 133, 13
    assert not r.reverted

def test_enums(harness):
    """abiEncoderV2/contracts/enums.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/enums.sol")
    # In-range enum values round-trip; values outside the enum range cause
    # the contract to revert. (256-bit dirty-byte cases aren't reachable
    # via the ARC4 client.)
    for v in (0, 1):
        assert as_int(harness.call(app, "f(uint8)", v).abi_return) == v
    assert harness.call(app, "f(uint8)", 2, expect_revert=True).reverted

def test_memory_dynamic_array_and_calldata_bytes(harness):
    """abiEncoderV2/contracts/memory_dynamic_array_and_calldata_bytes.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/memory_dynamic_array_and_calldata_bytes.sol")

    def encoded(a: list[int], b: bytes) -> bytes:
        head_a = 0x40
        head_b = head_a + 32 + 32 * len(a)
        body_a = len(a).to_bytes(32, "big") + b"".join(v.to_bytes(32, "big") for v in a)
        body_b = len(b).to_bytes(32, "big") + b.ljust(((len(b) + 31) // 32) * 32, b"\x00")
        return head_a.to_bytes(32, "big") + head_b.to_bytes(32, "big") + body_a + body_b

    assert bytes(harness.call(app, "f(uint256[],bytes)", [0xff], b"123456").abi_return) == encoded([0xff], b"123456")
    assert bytes(harness.call(app, "g(uint256[],bytes)", [0xffff], b"12345678").abi_return) == encoded([0xffff], b"12345678")

def test_memory_dynamic_array_and_calldata_static_array(harness):
    """abiEncoderV2/contracts/memory_dynamic_array_and_calldata_static_array.sol

    f/g return abi.encode(a, b) where a is uint256[] (dynamic, has head)
    and b is uint256[1] (static, inlined). EVM layout:
      [head_a=0x40][b[0]][length_a=1][a[0]]
    """
    app = harness.compile_and_deploy("abiEncoderV2/contracts/memory_dynamic_array_and_calldata_static_array.sol")
    expected = b"".join(v.to_bytes(32, "big") for v in (0x40, 0xffff, 1, 0xff))
    assert bytes(harness.call(app, "f(uint256[],uint256[1])", [0xff], [0xffff]).abi_return) == expected
    assert bytes(harness.call(app, "g(uint256[],uint256[1])", [0xff], [0xffff]).abi_return) == expected
    # h returns (a, b) — algosdk decodes as tuple ([0xff], [0xffff]).
    r = harness.call(app, "h(uint256[],uint256[1])", [0xff], [0xffff])
    a, b = r.abi_return
    assert [as_int(x) for x in a] == [0xff]
    assert [as_int(x) for x in b] == [0xffff]

def test_memory_params_in_external_function(harness):
    """abiEncoderV2/contracts/memory_params_in_external_function.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/memory_params_in_external_function.sol")
    # g() -> 3, 0x6200000000000000000000000000000000000000000000000000000000000000, 3, 0x6600000000000000000000000000000000000000000000000000000000000000, 4, 7
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 3, 44326659161160106060585767698638339725079916004815528421354856378029244940288, 3, 46135910555493171614079064339399088285287259515216162234471381128152887590912, 4, 7
    assert not r.reverted

def test_storage_array_encoding(harness):
    """abiEncoderV2/contracts/storage_array_encoding.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/contracts/storage_array_encoding.sol")
    # h: uint256[2][] → abi.encode = (offset=32, length=3, elements packed).
    s_h = [[123, 124], [223, 224], [323, 324]]
    expected_h = (
        (32).to_bytes(32, "big") + (3).to_bytes(32, "big")
        + b"".join(v.to_bytes(32, "big") for pair in s_h for v in pair)
    )
    assert bytes(harness.call(app, "h(uint256[2][])", s_h).abi_return) == expected_h
    # i: uint256[2][2] (fully static) → abi.encode = just the 4 packed values.
    s_i = [[123, 124], [223, 224]]
    expected_i = b"".join(v.to_bytes(32, "big") for pair in s_i for v in pair)
    assert bytes(harness.call(app, "i(uint256[2][2])", s_i).abi_return) == expected_i
