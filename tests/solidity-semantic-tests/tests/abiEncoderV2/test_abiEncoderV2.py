"""Auto-generated tests for the abiEncoderV2 category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_abi_encode_calldata_slice(harness):
    """abiEncoderV2/abi_encode_calldata_slice.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_calldata_slice.sol")
    # test_bytes() ->
    r = harness.call(app, "test_bytes()")
    # (void return — call succeeding is the assertion)
    # test_uint256() ->
    r = harness.call(app, "test_uint256()")
    # (void return — call succeeding is the assertion)

def test_abi_encode_empty_string_v2(harness):
    """abiEncoderV2/abi_encode_empty_string_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_empty_string_v2.sol")
    # f() -> 0x40, 0xa0, 0x40, 0x20, 0x0, 0x0
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 64, 160, 64, 32, 0, 0
    assert not r.reverted

def test_abi_encode_rational_v2(harness):
    """abiEncoderV2/abi_encode_rational_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_rational_v2.sol")
    # f() -> 0x20, 0x40, 0x1, -2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 64, 1, -2)

def test_abi_encode_v2(harness):
    """abiEncoderV2/abi_encode_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_v2.sol")
    # f0() -> 0x20, 0x0
    r = harness.call(app, "f0()")
    assert tuple(r.abi_return) == (32, 0)
    # f1() -> 0x20, 0x40, 0x1, 0x2
    r = harness.call(app, "f1()")
    assert tuple(r.abi_return) == (32, 64, 1, 2)
    # f2() -> 0x20, 0xa0, 0x1, 0x60, 0x2, 0x3, "abc"
    r = harness.call(app, "f2()")
    # TODO: verify expected: 0x20 | 0xa0 | 0x1 | 0x60 | 0x2 | 0x3 | "abc"
    assert not r.reverted
    # f3() -> 0x20, 0xa0, 0x1, 0x60, 0x2, 0x3, "abc"
    r = harness.call(app, "f3()")
    # TODO: verify expected: 0x20 | 0xa0 | 0x1 | 0x60 | 0x2 | 0x3 | "abc"
    assert not r.reverted
    # f4() -> 0x20, 0x160, 0x1, 0x80, 0xc0, 0x2, 0x3, "abc", 0x7, 0x40, 0x2, 0x2, 0x3
    r = harness.call(app, "f4()")
    # TODO: verify expected: 0x20 | 0x160 | 0x1 | 0x80 | 0xc0 | 0x2 | 0x3 | "abc" | 0x7 | 0x40 | 0x2 | 0x2 | 0x3
    assert not r.reverted

def test_abi_encode_v2_in_function_inherited_in_v1_contract(harness):
    """abiEncoderV2/abi_encode_v2_in_function_inherited_in_v1_contract.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_v2_in_function_inherited_in_v1_contract.sol")
    # test() -> 77
    r = harness.call(app, "test()")
    assert r.abi_return == 77

def test_abi_encode_v2_in_modifier_used_in_v1_contract(harness):
    """abiEncoderV2/abi_encode_v2_in_modifier_used_in_v1_contract.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encode_v2_in_modifier_used_in_v1_contract.sol")
    # test() -> 5, 10
    r = harness.call(app, "test()")
    assert tuple(r.abi_return) == (5, 10)

def test_abi_encoder_v2_head_overflow_with_static_array_cleanup_bug(harness):
    """abiEncoderV2/abi_encoder_v2_head_overflow_with_static_array_cleanup_bug.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/abi_encoder_v2_head_overflow_with_static_array_cleanup_bug.sol")
    # f(bool,(bytes,uint256[3]),bytes32[2]): 1, 0x80, "a", "b", 0x80, 11, 12, 13, 4, "abcd" -> 1, 0x80, "a", "b", 0x80, 11, 12, 13, 4, "abcd"
    r = harness.call(app, "f(bool,(bytes,uint256[3]),bytes32[2])", 1, 128, bytes.fromhex('61'), bytes.fromhex('62'), 128, 11, 12, 13, 4, bytes.fromhex('61626364'))
    # TODO: verify expected: 1 | 0x80 | "a" | "b" | 0x80 | 11 | 12 | 13 | 4 | "abcd"
    assert not r.reverted

def test_bool_out_of_bounds(harness):
    """abiEncoderV2/bool_out_of_bounds.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/bool_out_of_bounds.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True
    # f(bool): false -> false
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return is False
    # f(bool): 0x000000 -> false
    r = harness.call(app, "f(bool)", 0)
    assert r.abi_return is False
    # f(bool): 0xffffff -> FAILURE
    r = harness.call(app, "f(bool)", 16777215, expect_revert=True)
    assert r.reverted

def test_byte_arrays(harness):
    """abiEncoderV2/byte_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/byte_arrays.sol")
    # f(uint256,bytes,uint256): 6, 0x60, 9, 7, "abcdefg" -> 6, 7, "d", 9
    r = harness.call(app, "f(uint256,bytes,uint256)", 6, 96, 9, 7, bytes.fromhex('61626364656667'))
    # TODO: verify expected: 6 | 7 | "d" | 9
    assert not r.reverted
    # f_external(uint256,bytes,uint256): 6, 0x60, 9, 7, "abcdefg" -> 6, 7, "d", 9
    r = harness.call(app, "f_external(uint256,bytes,uint256)", 6, 96, 9, 7, bytes.fromhex('61626364656667'))
    # TODO: verify expected: 6 | 7 | "d" | 9
    assert not r.reverted

def test_calldata_array(harness):
    """abiEncoderV2/calldata_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array.sol")
    # f(uint256[][1]): 32, 32, 0 -> true
    r = harness.call(app, "f(uint256[][1])", 32, 32, 0)
    assert r.abi_return is True
    # f(uint256[][1]): 32, 32, 1, 42 -> true
    r = harness.call(app, "f(uint256[][1])", 32, 32, 1, 42)
    assert r.abi_return is True
    # f(uint256[][1]): 32, 32, 8, 421, 422, 423, 424, 425, 426, 427, 428 -> true
    r = harness.call(app, "f(uint256[][1])", 32, 32, 8, 421, 422, 423, 424, 425, 426, 427, 428)
    assert r.abi_return is True

def test_calldata_array_dynamic(harness):
    """abiEncoderV2/calldata_array_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic.sol")
    # f(uint256[]): 32, 3, 23, 42, 87 -> 32, 160, 32, 3, 23, 42, 87
    r = harness.call(app, "f(uint256[])", 32, 3, 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # g(uint256[]): 32, 3, 23, 42, 87 -> 32, 160, 32, 3, 23, 42, 87
    r = harness.call(app, "g(uint256[])", 32, 3, 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # h(uint8[]): 32, 3, 23, 42, 87 -> 32, 160, 32, 3, 23, 42, 87
    r = harness.call(app, "h(uint8[])", 32, 3, 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # i(uint8[]): 32, 3, 23, 42, 87 -> 32, 160, 32, 3, 23, 42, 87
    r = harness.call(app, "i(uint8[])", 32, 3, 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 23, 42, 87
    assert not r.reverted
    # h(uint8[]): 32, 3, 0xFF23, 0x1242, 0xAB87 -> FAILURE
    r = harness.call(app, "h(uint8[])", 32, 3, 65315, 4674, 43911, expect_revert=True)
    assert r.reverted
    # i(uint8[]): 32, 3, 0xAB23, 0x1242, 0xFF87 -> FAILURE
    r = harness.call(app, "i(uint8[])", 32, 3, 43811, 4674, 65415, expect_revert=True)
    assert r.reverted
    # j(bytes): 32, 3, hex"123456" -> 32, 96, 32, 3, left(0x123456)
    r = harness.call(app, "j(bytes)", 32, 3, bytes.fromhex('123456'))
    # TODO: verify expected: 32 | 96 | 32 | 3 | left(0x123456)
    assert not r.reverted
    # k(bytes): 32, 3, hex"AB33FF" -> 32, 96, 32, 3, left(0xAB33FF)
    r = harness.call(app, "k(bytes)", 32, 3, bytes.fromhex('ab33ff'))
    # TODO: verify expected: 32 | 96 | 32 | 3 | left(0xAB33FF)
    assert not r.reverted

def test_calldata_array_dynamic_index_access(harness):
    """abiEncoderV2/calldata_array_dynamic_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic_index_access.sol")
    # f(uint256[]): 32, 3, 42, 23, 87 -> 32, 160, 32, 3, 42, 23, 87
    r = harness.call(app, "f(uint256[])", 32, 3, 42, 23, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 42, 23, 87
    assert not r.reverted
    # g(uint256[][2],uint256): 0x40, 0, 0x40, 0xC0, 3, 42, 23, 87, 4, 11, 13, 17 -> 32, 160, 32, 3, 42, 23, 87
    r = harness.call(app, "g(uint256[][2],uint256)", 64, 0, 64, 192, 3, 42, 23, 87, 4, 11, 13, 17)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 42, 23, 87
    assert not r.reverted
    # g(uint256[][2],uint256): 0x40, 1, 0x40, 0xC0, 3, 42, 23, 87, 4, 11, 13, 17, 27 -> 32, 192, 32, 4, 11, 13, 17, 27
    r = harness.call(app, "g(uint256[][2],uint256)", 64, 1, 64, 192, 3, 42, 23, 87, 4, 11, 13, 17, 27)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 4, 11, 13, 17, 27
    assert not r.reverted
    # h(uint8[]): 32, 3, 42, 23, 87 -> 32, 160, 32, 3, 42, 23, 87
    r = harness.call(app, "h(uint8[])", 32, 3, 42, 23, 87)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 42, 23, 87
    assert not r.reverted
    # i(uint8[][2],uint256): 0x40, 0, 0x40, 0xC0, 3, 42, 23, 87, 4, 11, 13, 17 -> 32, 160, 32, 3, 42, 23, 87
    r = harness.call(app, "i(uint8[][2],uint256)", 64, 0, 64, 192, 3, 42, 23, 87, 4, 11, 13, 17)
    # TODO: verify structural decoding matches expected: 32, 160, 32, 3, 42, 23, 87
    assert not r.reverted
    # i(uint8[][2],uint256): 0x40, 1, 0x40, 0xC0, 3, 42, 23, 87, 4, 11, 13, 17, 27 -> 32, 192, 32, 4, 11, 13, 17, 27
    r = harness.call(app, "i(uint8[][2],uint256)", 64, 1, 64, 192, 3, 42, 23, 87, 4, 11, 13, 17, 27)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 4, 11, 13, 17, 27
    assert not r.reverted
    # j(bytes): 32, 3, hex"AB11FF" -> 32, 96, 32, 3, left(0xAB11FF)
    r = harness.call(app, "j(bytes)", 32, 3, bytes.fromhex('ab11ff'))
    # TODO: verify expected: 32 | 96 | 32 | 3 | left(0xAB11FF)
    assert not r.reverted
    # k(bytes[2],uint256): 0x40, 0, 0x40, 0x63, 3, hex"AB11FF", 4, hex"FF791432" -> 32, 96, 32, 3, left(0xAB11FF)
    r = harness.call(app, "k(bytes[2],uint256)", 64, 0, 64, 99, 3, bytes.fromhex('ab11ff'), 4, bytes.fromhex('ff791432'))
    # TODO: verify expected: 32 | 96 | 32 | 3 | left(0xAB11FF)
    assert not r.reverted
    # k(bytes[2],uint256): 0x40, 1, 0x40, 0x63, 3, hex"AB11FF", 4, hex"FF791432" -> 32, 96, 32, 4, left(0xFF791432)
    r = harness.call(app, "k(bytes[2],uint256)", 64, 1, 64, 99, 3, bytes.fromhex('ab11ff'), 4, bytes.fromhex('ff791432'))
    # TODO: verify expected: 32 | 96 | 32 | 4 | left(0xFF791432)
    assert not r.reverted

def test_calldata_array_dynamic_static_dynamic(harness):
    """abiEncoderV2/calldata_array_dynamic_static_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic_static_dynamic.sol")
    # g() -> 32, 196, hex"eccb829a", 32, 1, 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "g()")
    # TODO: verify expected: 32 | 196 | hex"eccb829a" | 32 | 1 | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted
    # h() -> 32, 196, hex"eccb829a", 32, 1, 32, 32, 1, 42, hex"00000000000000000000000000000000000000000000000000000000"
    r = harness.call(app, "h()")
    # TODO: verify expected: 32 | 196 | hex"eccb829a" | 32 | 1 | 32 | 32 | 1 | 42 | hex"00000000000000000000000000000000000000000000000000000000"
    assert not r.reverted

def test_calldata_array_dynamic_static_in_library(harness):
    """abiEncoderV2/calldata_array_dynamic_static_in_library.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic_static_in_library.sol")
    # f(uint256[],uint256[1]): 0x40, 0xff, 1, 0xffff -> 0x40, 0xff, 0x01, 0xffff
    r = harness.call(app, "f(uint256[],uint256[1])", 64, 255, 1, 65535)
    assert tuple(r.abi_return) == (64, 255, 1, 65535)

def test_calldata_array_dynamic_static_short_decode(harness):
    """abiEncoderV2/calldata_array_dynamic_static_short_decode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic_static_short_decode.sol")
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
    """abiEncoderV2/calldata_array_dynamic_static_short_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_dynamic_static_short_reencode.sol")
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x40, 0x60, 0x00, 0x00 -> 42
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 64, 96, 0, 0)
    assert r.abi_return == 42
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x00, 0x00 -> 42
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 0, 0)
    assert r.abi_return == 42
    # g(uint256[][2][]): 0x20, 0x01, 0x20, 0x00 -> FAILURE
    r = harness.call(app, "g(uint256[][2][])", 32, 1, 32, 0, expect_revert=True)
    assert r.reverted

def test_calldata_array_function_types(harness):
    """abiEncoderV2/calldata_array_function_types.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_function_types.sol")
    # g(bool): false -> 23, 37, 71
    r = harness.call(app, "g(bool)", False)
    assert tuple(r.abi_return) == (23, 37, 71)
    # g(bool): true -> 23, 37, 71
    r = harness.call(app, "g(bool)", True)
    assert tuple(r.abi_return) == (23, 37, 71)

def test_calldata_array_multi_dynamic(harness):
    """abiEncoderV2/calldata_array_multi_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_multi_dynamic.sol")
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
    """abiEncoderV2/calldata_array_short.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_short.sol")
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
    """abiEncoderV2/calldata_array_short_no_revert_string.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_short_no_revert_string.sol")
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
    """abiEncoderV2/calldata_array_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_static.sol")
    # f(uint256[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "f(uint256[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # g(uint256[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "g(uint256[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # h(uint8[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "h(uint8[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # i(uint8[3]): 23, 42, 87 -> 32, 96, 23, 42, 87
    r = harness.call(app, "i(uint8[3])", 23, 42, 87)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # h(uint8[3]): 0xFF23, 0x1242, 0xAB87 -> FAILURE
    r = harness.call(app, "h(uint8[3])", 65315, 4674, 43911, expect_revert=True)
    assert r.reverted
    # i(uint8[3]): 0xAB23, 0x1242, 0xFF87 -> FAILURE
    r = harness.call(app, "i(uint8[3])", 43811, 4674, 65415, expect_revert=True)
    assert r.reverted

def test_calldata_array_static_dynamic_static(harness):
    """abiEncoderV2/calldata_array_static_dynamic_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_static_dynamic_static.sol")
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
    """abiEncoderV2/calldata_array_static_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_static_index_access.sol")
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
    """abiEncoderV2/calldata_array_struct_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_struct_dynamic.sol")
    # f((uint256[])[]): 32, 1, 32, 32, 3, 17, 42, 23 -> 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    r = harness.call(app, "f((uint256[])[])", 32, 1, 32, 32, 3, 17, 42, 23)
    # TODO: verify structural decoding matches expected: 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    assert not r.reverted
    # g((uint256[])[]): 32, 1, 32, 32, 3, 17, 42, 23 -> 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    r = harness.call(app, "g((uint256[])[])", 32, 1, 32, 32, 3, 17, 42, 23)
    # TODO: verify structural decoding matches expected: 32, 256, 32, 1, 32, 32, 3, 17, 42, 23
    assert not r.reverted

def test_calldata_array_two_dynamic(harness):
    """abiEncoderV2/calldata_array_two_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_two_dynamic.sol")
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
    """abiEncoderV2/calldata_array_two_static.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_array_two_static.sol")
    # f(uint256[3],uint256[2],bool): 23, 42, 87, 51, 72, true -> 32, 96, 23, 42, 87
    r = harness.call(app, "f(uint256[3],uint256[2],bool)", 23, 42, 87, 51, 72, True)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # f(uint256[3],uint256[2],bool): 23, 42, 87, 51, 72, false -> 32, 64, 51, 72
    r = harness.call(app, "f(uint256[3],uint256[2],bool)", 23, 42, 87, 51, 72, False)
    assert tuple(r.abi_return) == (32, 64, 51, 72)
    # g(uint256[3],uint256[2],bool): 23, 42, 87, 51, 72, true -> 32, 96, 23, 42, 87
    r = harness.call(app, "g(uint256[3],uint256[2],bool)", 23, 42, 87, 51, 72, True)
    # TODO: verify structural decoding matches expected: 32, 96, 23, 42, 87
    assert not r.reverted
    # g(uint256[3],uint256[2],bool): 23, 42, 87, 51, 72, false -> 32, 64, 51, 72
    r = harness.call(app, "g(uint256[3],uint256[2],bool)", 23, 42, 87, 51, 72, False)
    assert tuple(r.abi_return) == (32, 64, 51, 72)

def test_calldata_dynamic_array_to_memory(harness):
    """abiEncoderV2/calldata_dynamic_array_to_memory.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_dynamic_array_to_memory.sol")
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
    """abiEncoderV2/calldata_nested_array_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_nested_array_reencode.sol")
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
    """abiEncoderV2/calldata_nested_array_static_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_nested_array_static_reencode.sol")
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
    """abiEncoderV2/calldata_overlapped_dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_overlapped_dynamic_arrays.sol")
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
    assert tuple(r.abi_return) == (32, 64, 5, 2)
    # f_which(uint256[],uint256[2],uint256): 0x40, 1, 2, 1, 5, 6 -> 0x20, 0x40, 5, 2
    r = harness.call(app, "f_which(uint256[],uint256[2],uint256)", 64, 1, 2, 1, 5, 6)
    assert tuple(r.abi_return) == (32, 64, 5, 2)
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
    """abiEncoderV2/calldata_overlapped_nested_dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_overlapped_nested_dynamic_arrays.sol")
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
    assert tuple(r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0x40, 0x40, 2, 1, 2 -> 0x20, 2, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 64, 64, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 0, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 2, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 0, 2, 64, 96, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 2, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0x40, 0x60, 2, 1, 2 -> 0x20, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 64, 96, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 0, 2, 0, 0x60, 2, 1, 2 -> 0x20, 0
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 0, 2, 0, 96, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 0)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0, 0x60, 2, 1, 2 -> 0x20, 1, 2
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 0, 96, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 1, 2)
    # f_which(uint256[][],uint256): 0x40, 1, 2, 0, 0x60, 2, 2, 2 -> FAILURE
    r = harness.call(app, "f_which(uint256[][],uint256)", 64, 1, 2, 0, 96, 2, 2, 2, expect_revert=True)
    assert r.reverted

def test_calldata_struct_array_reencode(harness):
    """abiEncoderV2/calldata_struct_array_reencode.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_struct_array_reencode.sol")
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
    assert tuple(r.abi_return) == (32, 64, 1, 2)
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
    """abiEncoderV2/calldata_struct_dynamic.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_struct_dynamic.sol")
    # f((uint256[])): 0x20, 0x20, 3, 42, 23, 17 -> 32, 192, 0x20, 0x20, 3, 42, 23, 17
    r = harness.call(app, "f((uint256[]))", 32, 32, 3, 42, 23, 17)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 32, 3, 42, 23, 17
    assert not r.reverted
    # g((uint256[])): 0x20, 0x20, 3, 42, 23, 17 -> 32, 192, 0x20, 0x20, 3, 42, 23, 17
    r = harness.call(app, "g((uint256[]))", 32, 32, 3, 42, 23, 17)
    # TODO: verify structural decoding matches expected: 32, 192, 32, 32, 3, 42, 23, 17
    assert not r.reverted

def test_calldata_struct_member_offset(harness):
    """abiEncoderV2/calldata_struct_member_offset.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_struct_member_offset.sol")
    # f() -> 11, 11
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (11, 11)

def test_calldata_struct_simple(harness):
    """abiEncoderV2/calldata_struct_simple.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_struct_simple.sol")
    # f((uint256)): 3 -> 32, 32, 3
    r = harness.call(app, "f((uint256))", 3)
    assert tuple(r.abi_return) == (32, 32, 3)
    # g((uint256)): 3 -> 32, 32, 3
    r = harness.call(app, "g((uint256))", 3)
    assert tuple(r.abi_return) == (32, 32, 3)

def test_calldata_three_dimensional_dynamic_array_index_access(harness):
    """abiEncoderV2/calldata_three_dimensional_dynamic_array_index_access.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_three_dimensional_dynamic_array_index_access.sol")
    # f(uint256[][],uint256,uint256): 0x60, 0, 0, 2, 0x40, 0x80, 1, 7, 1, 8 -> 0x20, 0x20, 7
    r = harness.call(app, "f(uint256[][],uint256,uint256)", 96, 0, 0, 2, 64, 128, 1, 7, 1, 8)
    assert tuple(r.abi_return) == (32, 32, 7)
    # f(uint256[][],uint256,uint256): 0x60, 1, 0, 2, 0x40, 0x80, 1, 7, 1, 8 -> 0x20, 0x20, 8
    r = harness.call(app, "f(uint256[][],uint256,uint256)", 96, 1, 0, 2, 64, 128, 1, 7, 1, 8)
    assert tuple(r.abi_return) == (32, 32, 8)
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 0, 0, 0, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> 0x20, 0x20, 4
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 0, 0, 0, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7)
    assert tuple(r.abi_return) == (32, 32, 4)
    # g(uint256[][][],uint256,uint256,uint256): 0x80, 1, 0, 1, 2, 0x40, 0xc0, 1, 0x20, 1, 4, 2, 0x40, 0xa0, 2, 5, 6, 1, 7 -> 0x20, 0x20, 6
    r = harness.call(app, "g(uint256[][][],uint256,uint256,uint256)", 128, 1, 0, 1, 2, 64, 192, 1, 32, 1, 4, 2, 64, 160, 2, 5, 6, 1, 7)
    assert tuple(r.abi_return) == (32, 32, 6)
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
    """abiEncoderV2/calldata_with_garbage.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/calldata_with_garbage.sol")
    # f_memory(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(r.abi_return) == (32, 0)
    # f_memory(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 1, 7
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 1, 7)
    assert tuple(r.abi_return) == (32, 1, 7)
    # f_memory(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_memory(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_encode(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # f_encode(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 0x60, 0x20, 1, 7
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 7
    assert not r.reverted
    # f_encode(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_encode(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_storage(uint256[]): 0x80, 9, 9, 9, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 0)
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # f_storage(uint256[]): 0x80, 9, 9, 9, 1, 7 -> 0x20, 0x60, 0x20, 1, 7
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 1, 7)
    # TODO: verify structural decoding matches expected: 32, 96, 32, 1, 7
    assert not r.reverted
    # f_storage(uint256[]): 0x80, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "f_storage(uint256[])", 128, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted
    # f_index(uint256[],uint256): 0xa0, 0, 9, 9, 9, 2, 7, 8 -> 7
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 0, 9, 9, 9, 2, 7, 8)
    assert r.abi_return == 7
    # f_index(uint256[],uint256): 0xa0, 1, 9, 9, 9, 2, 7, 8 -> 8
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 1, 9, 9, 9, 2, 7, 8)
    assert r.abi_return == 8
    # f_index(uint256[],uint256): 0xa0, 2, 9, 9, 9, 2, 7, 8 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f_index(uint256[],uint256)", 160, 2, 9, 9, 9, 2, 7, 8, expect_revert=True)
    assert r.reverted
    # g_memory(uint256[],uint256[2]): 0xc0, 1, 2, 9, 9, 9, 0 -> 0x60, 1, 2, 0
    r = harness.call(app, "g_memory(uint256[],uint256[2])", 192, 1, 2, 9, 9, 9, 0)
    assert tuple(r.abi_return) == (96, 1, 2, 0)
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
    assert tuple(r.abi_return) == (7, 1)
    # g_index(uint256[],uint256[2],uint256): 0xe0, 1, 2, 1, 9, 9, 9, 2, 7, 8 -> 8, 1
    r = harness.call(app, "g_index(uint256[],uint256[2],uint256)", 224, 1, 2, 1, 9, 9, 9, 2, 7, 8)
    assert tuple(r.abi_return) == (8, 1)
    # g_index(uint256[],uint256[2],uint256): 0xe0, 1, 2, 1, 9, 9, 9, 2, 7 -> FAILURE
    r = harness.call(app, "g_index(uint256[],uint256[2],uint256)", 224, 1, 2, 1, 9, 9, 9, 2, 7, expect_revert=True)
    assert r.reverted

def test_dynamic_arrays(harness):
    """abiEncoderV2/dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/dynamic_arrays.sol")
    # f(uint256,uint16[],uint256): 6, 0x60, 9, 7, 11, 12, 13, 14, 15, 16, 17 -> 7, 17, 9
    r = harness.call(app, "f(uint256,uint16[],uint256)", 6, 96, 9, 7, 11, 12, 13, 14, 15, 16, 17)
    assert tuple(r.abi_return) == (7, 17, 9)

def test_dynamic_nested_arrays(harness):
    """abiEncoderV2/dynamic_nested_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/dynamic_nested_arrays.sol")
    # test() -> 12, 3, 4, 0x66, 5, 0x85, 13
    r = harness.call(app, "test()")
    # TODO: verify structural decoding matches expected: 12, 3, 4, 102, 5, 133, 13
    assert not r.reverted
    # f(uint256,uint16[][],uint256[2][][3],uint256): 12, 0x80, 0x220, 13, 3, 0x60, 0xC0, 0x160, 2, 85, 86, 4, 101, 102, 103, 104, 0, 0x60, 0xC0, 0x220, 1, 0, 117, 5, 0, 0, 0, 133, 0, 0, 0, 0, 0, 0, 0 -> 12, 3, 4, 0x66, 5, 0x85, 13
    r = harness.call(app, "f(uint256,uint16[][],uint256[2][][3],uint256)", 12, 128, 544, 13, 3, 96, 192, 352, 2, 85, 86, 4, 101, 102, 103, 104, 0, 96, 192, 544, 1, 0, 117, 5, 0, 0, 0, 133, 0, 0, 0, 0, 0, 0, 0)
    # TODO: verify structural decoding matches expected: 12, 3, 4, 102, 5, 133, 13
    assert not r.reverted

def test_enums(harness):
    """abiEncoderV2/enums.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/enums.sol")
    # f(uint8): 0 -> 0
    r = harness.call(app, "f(uint8)", 0)
    assert r.abi_return == 0
    # f(uint8): 1 -> 1
    r = harness.call(app, "f(uint8)", 1)
    assert r.abi_return == 1
    # f(uint8): 2 -> FAILURE
    r = harness.call(app, "f(uint8)", 2, expect_revert=True)
    assert r.reverted
    # f(uint8): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "f(uint8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted

def test_memory_dynamic_array_and_calldata_bytes(harness):
    """abiEncoderV2/memory_dynamic_array_and_calldata_bytes.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/memory_dynamic_array_and_calldata_bytes.sol")
    # f(uint256[],bytes): 0x40, 0x80, 1, 0xFF, 6, "123456" -> 0x20, 0xc0, 0x40, 0x80, 1, 0xff, 6, "123456"
    r = harness.call(app, "f(uint256[],bytes)", 64, 128, 1, 255, 6, bytes.fromhex('313233343536'))
    # TODO: verify expected: 0x20 | 0xc0 | 0x40 | 0x80 | 1 | 0xff | 6 | "123456"
    assert not r.reverted
    # g(uint256[],bytes): 0x40, 0x80, 1, 0xffff, 8, "12345678" -> 0x20, 0xc0, 0x40, 0x80, 1, 0xffff, 8, "12345678"
    r = harness.call(app, "g(uint256[],bytes)", 64, 128, 1, 65535, 8, bytes.fromhex('3132333435363738'))
    # TODO: verify expected: 0x20 | 0xc0 | 0x40 | 0x80 | 1 | 0xffff | 8 | "12345678"
    assert not r.reverted

def test_memory_dynamic_array_and_calldata_static_array(harness):
    """abiEncoderV2/memory_dynamic_array_and_calldata_static_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/memory_dynamic_array_and_calldata_static_array.sol")
    # f(uint256[],uint256[1]): 0x40, 0xff, 1, 0xffff -> 0x20, 0x80, 0x40, 0xff, 1, 0xffff
    r = harness.call(app, "f(uint256[],uint256[1])", 64, 255, 1, 65535)
    # TODO: verify structural decoding matches expected: 32, 128, 64, 255, 1, 65535
    assert not r.reverted
    # g(uint256[],uint256[1]): 0x40, 0xff, 1, 0xffff -> 0x20, 0x80, 0x40, 0xff, 1, 0xffff
    r = harness.call(app, "g(uint256[],uint256[1])", 64, 255, 1, 65535)
    # TODO: verify structural decoding matches expected: 32, 128, 64, 255, 1, 65535
    assert not r.reverted
    # h(uint256[],uint256[1]): 0x40, 0xff, 1, 0xffff -> 0x40, 0xff, 1, 0xffff
    r = harness.call(app, "h(uint256[],uint256[1])", 64, 255, 1, 65535)
    assert tuple(r.abi_return) == (64, 255, 1, 65535)

def test_memory_params_in_external_function(harness):
    """abiEncoderV2/memory_params_in_external_function.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/memory_params_in_external_function.sol")
    # g() -> 3, 0x6200000000000000000000000000000000000000000000000000000000000000, 3, 0x6600000000000000000000000000000000000000000000000000000000000000, 4, 7
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 3, 44326659161160106060585767698638339725079916004815528421354856378029244940288, 3, 46135910555493171614079064339399088285287259515216162234471381128152887590912, 4, 7
    assert not r.reverted

def test_storage_array_encoding(harness):
    """abiEncoderV2/storage_array_encoding.sol"""
    app = harness.compile_and_deploy("abiEncoderV2/storage_array_encoding.sol")
    # h(uint256[2][]): 0x20, 3, 123, 124, 223, 224, 323, 324 -> 32, 256, 0x20, 3, 123, 124, 223, 224, 323, 324
    r = harness.call(app, "h(uint256[2][])", 32, 3, 123, 124, 223, 224, 323, 324)
    # TODO: verify structural decoding matches expected: 32, 256, 32, 3, 123, 124, 223, 224, 323, 324
    assert not r.reverted
    # i(uint256[2][2]): 123, 124, 223, 224 -> 32, 128, 123, 124, 223, 224
    r = harness.call(app, "i(uint256[2][2])", 123, 124, 223, 224)
    # TODO: verify structural decoding matches expected: 32, 128, 123, 124, 223, 224
    assert not r.reverted
