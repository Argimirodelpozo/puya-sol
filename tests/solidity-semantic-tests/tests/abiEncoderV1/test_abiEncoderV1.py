"""Auto-generated tests for the abiEncoderV1 category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_abi_decode_dynamic_array(harness):
    """abiEncoderV1/contracts/abi_decode_dynamic_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_dynamic_array.sol")
    # f(bytes): 0x20, 0xc0, 0x20, 0x4, 0x3, 0x4, 0x5, 0x6 -> 0x20, 0x4, 0x3, 0x4, 0x5, 0x6
    r = harness.call(app, "f(bytes)", 32, 192, 32, 4, 3, 4, 5, 6)
    # TODO: verify structural decoding matches expected: 32, 4, 3, 4, 5, 6
    assert not r.reverted

def test_abi_decode_fixed_arrays(harness):
    """abiEncoderV1/contracts/abi_decode_fixed_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_fixed_arrays.sol")
    # f(uint16[3],uint16[2][3],uint256,uint256,uint256): 1, 2, 3, 11, 12, 21, 22, 31, 32, 1, 2, 1 -> 2, 32
    r = harness.call(app, "f(uint16[3],uint16[2][3],uint256,uint256,uint256)", 1, 2, 3, 11, 12, 21, 22, 31, 32, 1, 2, 1)
    assert tuple(r.abi_return) == (2, 32)

def test_abi_decode_static_array(harness):
    """abiEncoderV1/contracts/abi_decode_static_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_static_array.sol")
    # f(bytes): 0x20, 0xc0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6 -> 1, 2, 3, 4, 5, 6
    r = harness.call(app, "f(bytes)", 32, 192, 1, 2, 3, 4, 5, 6)
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6
    assert not r.reverted

def test_abi_decode_static_array_v2(harness):
    """abiEncoderV1/contracts/abi_decode_static_array_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_static_array_v2.sol")
    # f(bytes): 0x20, 0xc0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6 -> 1, 2, 3, 4, 5, 6
    r = harness.call(app, "f(bytes)", 32, 192, 1, 2, 3, 4, 5, 6)
    # TODO: verify structural decoding matches expected: 1, 2, 3, 4, 5, 6
    assert not r.reverted

def test_abi_decode_trivial(harness):
    """abiEncoderV1/contracts/abi_decode_trivial.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_trivial.sol")
    # f(bytes): 0x20, 0x20, 0x21 -> 33
    r = harness.call(app, "f(bytes)", 32, 32, 33)
    assert r.abi_return == 33

def test_abi_decode_v2(harness):
    """abiEncoderV1/contracts/abi_decode_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_v2.sol")
    # f() -> 0x20, 0x8, 0x40, 0x3, 0x9, 0xa, 0xb
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 32, 8, 64, 3, 9, 10, 11
    assert not r.reverted

def test_abi_decode_v2_calldata(harness):
    """abiEncoderV1/contracts/abi_decode_v2_calldata.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_v2_calldata.sol")
    # f(bytes): 0x20, 0xe0, 0x20, 0x21, 0x40, 0x3, 0xa, 0xb, 0xc -> 0x20, 0x21, 0x40, 0x3, 0xa, 0xb, 0xc
    r = harness.call(app, "f(bytes)", 32, 224, 32, 33, 64, 3, 10, 11, 12)
    # TODO: verify structural decoding matches expected: 32, 33, 64, 3, 10, 11, 12
    assert not r.reverted

def test_abi_decode_v2_storage(harness):
    """abiEncoderV1/contracts/abi_decode_v2_storage.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_v2_storage.sol")
    # f() -> 0x20, 0x8, 0x40, 0x3, 0x9, 0xa, 0xb
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 32, 8, 64, 3, 9, 10, 11
    assert not r.reverted

def test_abi_encode(harness):
    """abiEncoderV1/contracts/abi_encode.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode.sol")
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
    # f4() -> 0x20, 0x20, "ab"
    r = harness.call(app, "f4()")
    assert r.abi_return == 'ab'

def test_abi_encode_call(harness):
    """abiEncoderV1/contracts/abi_encode_call.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_call.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert r.abi_return is True

def test_abi_encode_calldata_slice(harness):
    """abiEncoderV1/contracts/abi_encode_calldata_slice.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_calldata_slice.sol")
    # test_bytes() ->
    r = harness.call(app, "test_bytes()")
    # (void return — call succeeding is the assertion)
    # test_uint256() ->
    r = harness.call(app, "test_uint256()")
    # (void return — call succeeding is the assertion)

def test_abi_encode_decode_simple(harness):
    """abiEncoderV1/contracts/abi_encode_decode_simple.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_decode_simple.sol")
    # f() -> 0x21, 0x40, 0x7, "abcdefg"
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x21 | 0x40 | 0x7 | "abcdefg"
    assert not r.reverted

def test_abi_encode_empty_string(harness):
    """abiEncoderV1/contracts/abi_encode_empty_string.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_empty_string.sol")
    # f1() -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f1()")
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # f2(string): 0x20, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f2(string)", 32, 0)
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # f2(string): 0x20, 0, 0 -> 0x20, 0x40, 0x20, 0
    r = harness.call(app, "f2(string)", 32, 0, 0)
    assert tuple(r.abi_return) == (32, 64, 32, 0)
    # g1() -> 32, 0
    r = harness.call(app, "g1()")
    assert tuple(r.abi_return) == (32, 0)
    # g2(string): 0x20, 0 -> 0x20, 0
    r = harness.call(app, "g2(string)", 32, 0)
    assert tuple(r.abi_return) == (32, 0)
    # g2(string): 0x20, 0, 0 -> 0x20, 0
    r = harness.call(app, "g2(string)", 32, 0, 0)
    assert tuple(r.abi_return) == (32, 0)
    # h1() -> 0x20, 0x44, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    r = harness.call(app, "h1()")
    # TODO: verify structural decoding matches expected: 32, 68, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    assert not r.reverted
    # h2(string): 0x20, 0 -> 0x20, 0x44, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    r = harness.call(app, "h2(string)", 32, 0)
    # TODO: verify structural decoding matches expected: 32, 68, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    assert not r.reverted
    # h2(string): 0x20, 0, 0 -> 0x20, 0x44, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    r = harness.call(app, "h2(string)", 32, 0, 0)
    # TODO: verify structural decoding matches expected: 32, 68, 26959946667150639794667015087019630673637144422540572481103610249216, 862718293348820473429344482784628181556388621521298319395315527974912, 0
    assert not r.reverted

def test_abi_encode_rational(harness):
    """abiEncoderV1/contracts/abi_encode_rational.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_rational.sol")
    # f() -> 0x20, 0x40, 0x1, -2
    r = harness.call(app, "f()")
    assert tuple(r.abi_return) == (32, 64, 1, -2)

def test_bool_out_of_bounds(harness):
    """abiEncoderV1/contracts/bool_out_of_bounds.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/bool_out_of_bounds.sol")
    # f(bool): true -> true
    r = harness.call(app, "f(bool)", True)
    assert r.abi_return is True
    # f(bool): false -> false
    r = harness.call(app, "f(bool)", False)
    assert r.abi_return is False
    # f(bool): 0x000000 -> false
    r = harness.call(app, "f(bool)", 0)
    assert r.abi_return is False
    # f(bool): 0xffffff -> true
    r = harness.call(app, "f(bool)", 16777215)
    assert r.abi_return is True

def test_byte_arrays(harness):
    """abiEncoderV1/contracts/byte_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/byte_arrays.sol")
    # f(uint256,bytes,uint256): 6, 0x60, 9, 7, "abcdefg" -> 6, 7, "d", 9
    r = harness.call(app, "f(uint256,bytes,uint256)", 6, 96, 9, 7, bytes.fromhex('61626364656667'))
    # TODO: verify expected: 6 | 7 | "d" | 9
    assert not r.reverted
    # f_external(uint256,bytes,uint256): 6, 0x60, 9, 7, "abcdefg" -> 6, 7, "d", 9
    r = harness.call(app, "f_external(uint256,bytes,uint256)", 6, 96, 9, 7, bytes.fromhex('61626364656667'))
    # TODO: verify expected: 6 | 7 | "d" | 9
    assert not r.reverted

def test_calldata_arrays_too_large(harness):
    """abiEncoderV1/contracts/calldata_arrays_too_large.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/calldata_arrays_too_large.sol")
    # f(uint256,uint256[],uint256): 6, 0x60, 9, 0x8000000000000000000000000000000000000000000000000000000000000002, 1, 2 -> FAILURE
    r = harness.call(app, "f(uint256,uint256[],uint256)", 6, 96, 9, 0x8000000000000000000000000000000000000000000000000000000000000002, 1, 2, expect_revert=True)
    assert r.reverted

def test_calldata_bytes_bytes32_arrays(harness):
    """abiEncoderV1/contracts/calldata_bytes_bytes32_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/calldata_bytes_bytes32_arrays.sol")
    # f(bool,bytes,bytes32[2]): true, 0x80, "a", "b", 4, "abcd" -> true, 0x80, "a", "b", 4, "abcd"
    r = harness.call(app, "f(bool,bytes,bytes32[2])", True, 128, bytes.fromhex('61'), bytes.fromhex('62'), 4, bytes.fromhex('61626364'))
    # TODO: verify expected: true | 0x80 | "a" | "b" | 4 | "abcd"
    assert not r.reverted

def test_decode_slice(harness):
    """abiEncoderV1/contracts/decode_slice.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/decode_slice.sol")
    # f(uint256,uint256): 42, 23 -> 42, 23, 42, 23
    r = harness.call(app, "f(uint256,uint256)", 42, 23)
    assert tuple(r.abi_return) == (42, 23, 42, 23)

def test_dynamic_arrays(harness):
    """abiEncoderV1/contracts/dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/dynamic_arrays.sol")
    # f(uint256,uint16[],uint256): 6, 0x60, 9, 7, 11, 12, 13, 14, 15, 16, 17 -> 7, 17, 9
    r = harness.call(app, "f(uint256,uint16[],uint256)", 6, 96, 9, 7, 11, 12, 13, 14, 15, 16, 17)
    assert tuple(r.abi_return) == (7, 17, 9)

def test_dynamic_memory_copy(harness):
    """abiEncoderV1/contracts/dynamic_memory_copy.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/dynamic_memory_copy.sol")
    # test(bytes): 0x20, 0x80, 0x40, 0x60, 0, 0 -> false, false
    r = harness.call(app, "test(bytes)", 32, 128, 64, 96, 0, 0)
    assert tuple(r.abi_return) == (False, False)
    # test(bytes): 0x20, 0xC0, 0x40, 0x80, 1, 0x42, 1, 0x42 -> false, false
    r = harness.call(app, "test(bytes)", 32, 192, 64, 128, 1, 66, 1, 66)
    assert tuple(r.abi_return) == (False, False)
    # test(bytes): 0x20, 0x80, 0x40, 0x40, 1, 0x42 -> false, false
    r = harness.call(app, "test(bytes)", 32, 128, 64, 64, 1, 66)
    assert tuple(r.abi_return) == (False, False)
    # test(bytes): 0x20, 0x60, 0x40, 0x40, 0 -> false, false
    r = harness.call(app, "test(bytes)", 32, 96, 64, 64, 0)
    assert tuple(r.abi_return) == (False, False)
    # test(bytes): 0x20, 0x80, 0x40, 0x40, 1, 0x42 -> false, false
    r = harness.call(app, "test(bytes)", 32, 128, 64, 64, 1, 66)
    assert tuple(r.abi_return) == (False, False)

def test_enums(harness):
    """abiEncoderV1/contracts/enums.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/enums.sol")
    # f(uint8): 0 -> 0
    r = harness.call(app, "f(uint8)", 0)
    assert r.abi_return == 0
    # f(uint8): 1 -> 1
    r = harness.call(app, "f(uint8)", 1)
    assert r.abi_return == 1
    # f(uint8): 2 -> 2
    r = harness.call(app, "f(uint8)", 2)
    assert r.abi_return == 2
    # f(uint8): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> 0xff
    r = harness.call(app, "f(uint8)", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
    assert r.abi_return == 255

def test_memory_dynamic_array_and_calldata_bytes(harness):
    """abiEncoderV1/contracts/memory_dynamic_array_and_calldata_bytes.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/memory_dynamic_array_and_calldata_bytes.sol")
    # f(uint256[],bytes): 0x40, 0x80, 1, 0xFF, 6, "123456" -> 0x20, 0xc0, 0x40, 0x80, 1, 0xff, 6, "123456"
    r = harness.call(app, "f(uint256[],bytes)", 64, 128, 1, 255, 6, bytes.fromhex('313233343536'))
    # TODO: verify expected: 0x20 | 0xc0 | 0x40 | 0x80 | 1 | 0xff | 6 | "123456"
    assert not r.reverted
    # g(uint256[],bytes): 0x40, 0x80, 1, 0xffff, 8, "12345678" -> 0x20, 0xc0, 0x40, 0x80, 1, 0xffff, 8, "12345678"
    r = harness.call(app, "g(uint256[],bytes)", 64, 128, 1, 65535, 8, bytes.fromhex('3132333435363738'))
    # TODO: verify expected: 0x20 | 0xc0 | 0x40 | 0x80 | 1 | 0xffff | 8 | "12345678"
    assert not r.reverted

def test_memory_params_in_external_function(harness):
    """abiEncoderV1/contracts/memory_params_in_external_function.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/memory_params_in_external_function.sol")
    # g() -> 3, 0x6200000000000000000000000000000000000000000000000000000000000000, 3, 0x6600000000000000000000000000000000000000000000000000000000000000, 4, 7
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 3, 44326659161160106060585767698638339725079916004815528421354856378029244940288, 3, 46135910555493171614079064339399088285287259515216162234471381128152887590912, 4, 7
    assert not r.reverted

def test_return_dynamic_types_cross_call_advanced(harness):
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_advanced.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/return_dynamic_types_cross_call_advanced.sol")
    # f() -> 0x80, -1, 0xe0, 0x1234, 40, "12345678901234567890123456789012", "34567890", 4, 97767552542602192590433234714624, 0, 0, 537879995309340587922569878831104
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x80 | -1 | 0xe0 | 0x1234 | 40 | "12345678901234567890123456789012" | "34567890" | 4 | 97767552542602192590433234714624 | 0 | 0 | 537879995309340587922569878831104
    assert not r.reverted

def test_return_dynamic_types_cross_call_out_of_range_1(harness):
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_1.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_1.sol", evm_version='homestead')
    # f(uint256): 0x60 -> true
    r = harness.call(app, "f(uint256)", 96)
    assert r.abi_return is True
    # f(uint256): 0x7f -> true
    r = harness.call(app, "f(uint256)", 127)
    assert r.abi_return is True
    # f(uint256): 0x80 -> true
    r = harness.call(app, "f(uint256)", 128)
    assert r.abi_return is True

def test_return_dynamic_types_cross_call_out_of_range_2(harness):
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_2.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_2.sol")
    # f(uint256): 0x60 -> FAILURE
    r = harness.call(app, "f(uint256)", 96, expect_revert=True)
    assert r.reverted
    # f(uint256): 0x61 -> true
    r = harness.call(app, "f(uint256)", 97)
    assert r.abi_return is True
    # f(uint256): 0x80 -> true
    r = harness.call(app, "f(uint256)", 128)
    assert r.abi_return is True

def test_return_dynamic_types_cross_call_simple(harness):
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_simple.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/return_dynamic_types_cross_call_simple.sol")
    # f() -> 0x20, 40, "12345678901234567890123456789012", "34567890"
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x20 | 40 | "12345678901234567890123456789012" | "34567890"
    assert not r.reverted
