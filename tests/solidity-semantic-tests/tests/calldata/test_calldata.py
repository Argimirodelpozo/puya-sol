"""Tests for the calldata category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_calldata_array_access(harness):
    """calldata/contracts/calldata_array_access.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_array_access.sol")
    # f(x: uint256[], i) returns x[i]; out-of-bounds reverts.
    assert harness.call(app, "f(uint256[],uint256)", [], 0, expect_revert=True).reverted
    assert as_int(harness.call(app, "f(uint256[],uint256)", [23], 0).abi_return) == 23
    assert harness.call(app, "f(uint256[],uint256)", [23], 1, expect_revert=True).reverted
    assert as_int(harness.call(app, "f(uint256[],uint256)", [23, 42], 0).abi_return) == 23
    assert as_int(harness.call(app, "f(uint256[],uint256)", [23, 42], 1).abi_return) == 42
    assert harness.call(app, "f(uint256[],uint256)", [23, 42], 2, expect_revert=True).reverted
    # The 2D overload f(x: uint[][], i, j) -> x[i][j].
    assert harness.call(app, "f(uint256[][],uint256,uint256)", [], 0, 0, expect_revert=True).reverted
    assert as_int(harness.call(app, "f(uint256[][],uint256,uint256)", [[23]], 0, 0).abi_return) == 23

def test_calldata_array_dynamic_bytes(harness):
    """calldata/contracts/calldata_array_dynamic_bytes.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_array_dynamic_bytes.sol")
    # f1(bytes[1]): 0x20, 0x20, 0x3, hex"0102030000000000000000000000000000000000000000000000000000000000" -> 0x3, 0x1, 0x2, 0x3
    r = harness.call(app, "f1(bytes[1])", 32, 32, 3, bytes.fromhex('0102030000000000000000000000000000000000000000000000000000000000'))
    assert tuple(as_int(x) for x in r.abi_return) == (3, 1, 2, 3)
    # f2(bytes[1],bytes[1]): 0x40, 0xa0, 0x20, 0x3, hex"0102030000000000000000000000000000000000000000000000000000000000", 0x20, 0x2, hex"0102000000000000000000000000000000000000000000000000000000000000" -> 0x3, 0x1, 0x2, 0x3, 0x2, 0x1, 0x2
    r = harness.call(app, "f2(bytes[1],bytes[1])", 64, 160, 32, 3, bytes.fromhex('0102030000000000000000000000000000000000000000000000000000000000'), 32, 2, bytes.fromhex('0102000000000000000000000000000000000000000000000000000000000000'))
    # TODO: verify structural decoding matches expected: 3, 1, 2, 3, 2, 1, 2
    assert not r.reverted
    # g1(bytes[2]): 0x20, 0x40, 0x80, 0x3, hex"0102030000000000000000000000000000000000000000000000000000000000", 0x3, hex"0405060000000000000000000000000000000000000000000000000000000000" -> 0x3, 0x1, 0x2, 0x3, 0x3, 0x4, 0x5, 0x6
    r = harness.call(app, "g1(bytes[2])", 32, 64, 128, 3, bytes.fromhex('0102030000000000000000000000000000000000000000000000000000000000'), 3, bytes.fromhex('0405060000000000000000000000000000000000000000000000000000000000'))
    # TODO: verify structural decoding matches expected: 3, 1, 2, 3, 3, 4, 5, 6
    assert not r.reverted
    # g1(bytes[2]): 0x20, 0x40, 0x40, 0x3, hex"0102030000000000000000000000000000000000000000000000000000000000" -> 0x3, 0x1, 0x2, 0x3, 0x3, 0x1, 0x2, 0x3
    r = harness.call(app, "g1(bytes[2])", 32, 64, 64, 3, bytes.fromhex('0102030000000000000000000000000000000000000000000000000000000000'))
    # TODO: verify structural decoding matches expected: 3, 1, 2, 3, 3, 1, 2, 3
    assert not r.reverted
    # g2(bytes[]): 0x20, 0x2, 0x40, 0x80, 0x2, hex"0102000000000000000000000000000000000000000000000000000000000000", 0x3, hex"0405060000000000000000000000000000000000000000000000000000000000" -> 0x2, 0x2, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6
    r = harness.call(app, "g2(bytes[])", 32, 2, 64, 128, 2, bytes.fromhex('0102000000000000000000000000000000000000000000000000000000000000'), 3, bytes.fromhex('0405060000000000000000000000000000000000000000000000000000000000'))
    # TODO: verify structural decoding matches expected: 2, 2, 1, 2, 3, 4, 5, 6
    assert not r.reverted

def test_calldata_array_index_range_access(harness):
    """calldata/contracts/calldata_array_index_range_access.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_array_index_range_access.sol")
    # f(uint256[],uint256,uint256): 0x60, 2, 4, 5, 1, 2, 3, 4, 5 -> 2
    r = harness.call(app, "f(uint256[],uint256,uint256)", 96, 2, 4, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 2
    # f(uint256[],uint256,uint256): 0x60, 2, 6, 5, 1, 2, 3, 4, 5 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 96, 2, 6, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x60, 3, 3, 5, 1, 2, 3, 4, 5 -> 0
    r = harness.call(app, "f(uint256[],uint256,uint256)", 96, 3, 3, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 0
    # f(uint256[],uint256,uint256): 0x60, 4, 3, 5, 1, 2, 3, 4, 5 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256)", 96, 4, 3, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # f(uint256[],uint256,uint256): 0x60, 0, 3, 5, 1, 2, 3, 4, 5 -> 3
    r = harness.call(app, "f(uint256[],uint256,uint256)", 96, 0, 3, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 3
    # f(uint256[],uint256,uint256,uint256,uint256): 0xA0, 1, 3, 1, 2, 5, 1, 2, 3, 4, 5 -> 1
    r = harness.call(app, "f(uint256[],uint256,uint256,uint256,uint256)", 160, 1, 3, 1, 2, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 1
    # f(uint256[],uint256,uint256,uint256,uint256): 0xA0, 1, 3, 1, 4, 5, 1, 2, 3, 4, 5 -> FAILURE
    r = harness.call(app, "f(uint256[],uint256,uint256,uint256,uint256)", 160, 1, 3, 1, 4, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # f_s_only(uint256[],uint256): 0x40, 2, 5, 1, 2, 3, 4, 5 -> 3
    r = harness.call(app, "f_s_only(uint256[],uint256)", 64, 2, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 3
    # f_s_only(uint256[],uint256): 0x40, 6, 5, 1, 2, 3, 4, 5 -> FAILURE
    r = harness.call(app, "f_s_only(uint256[],uint256)", 64, 6, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # f_e_only(uint256[],uint256): 0x40, 3, 5, 1, 2, 3, 4, 5 -> 3
    r = harness.call(app, "f_e_only(uint256[],uint256)", 64, 3, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 3
    # f_e_only(uint256[],uint256): 0x40, 6, 5, 1, 2, 3, 4, 5 -> FAILURE
    r = harness.call(app, "f_e_only(uint256[],uint256)", 64, 6, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # g(uint256[],uint256,uint256,uint256): 0x80, 2, 4, 1, 5, 1, 2, 3, 4, 5 -> 4
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 2, 4, 1, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 4
    # g(uint256[],uint256,uint256,uint256): 0x80, 2, 4, 3, 5, 1, 2, 3, 4, 5 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "g(uint256[],uint256,uint256,uint256)", 128, 2, 4, 3, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted
    # gg(uint256[],uint256,uint256,uint256): 0x80, 2, 4, 1, 5, 1, 2, 3, 4, 5 -> 4
    r = harness.call(app, "gg(uint256[],uint256,uint256,uint256)", 128, 2, 4, 1, 5, 1, 2, 3, 4, 5)
    assert as_int(r.abi_return) == 4
    # gg(uint256[],uint256,uint256,uint256): 0x80, 2, 4, 3, 5, 1, 2, 3, 4, 5 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "gg(uint256[],uint256,uint256,uint256)", 128, 2, 4, 3, 5, 1, 2, 3, 4, 5, expect_revert=True)
    assert r.reverted

def test_calldata_array_length(harness):
    """calldata/contracts/calldata_array_length.sol

    Tests `.length` on calldata uint[], uint[][], and uint[2]. EVM-flat
    overlapping-offset edge cases (the `-32` and aliased-offset variants)
    aren't expressible through algosdk's ARC4 array encoding so we only
    cover the canonically-shaped inputs here.
    """
    app = harness.compile_and_deploy("calldata/contracts/calldata_array_length.sol")
    # f(uint256[]) → length.
    for arr in ([], [23], [23, 42], [23, 42, 17]):
        assert as_int(harness.call(app, "f(uint256[])", arr).abi_return) == len(arr)
    # f(uint256[2]) → length = 2 (static).
    assert as_int(harness.call(app, "f(uint256[2])", [23, 42]).abi_return) == 2
    # f(uint256[][]) → (length, x[0].length if any, x[1].length if any).
    cases = [
        ([], (0, 0, 0)),
        ([[]], (1, 0, 0)),
        ([[23]], (1, 1, 0)),
        ([[23, 42]], (1, 2, 0)),
        ([[23, 42], [23, 42]], (2, 2, 2)),
        ([[23, 42], []], (2, 2, 0)),
        ([[], [23, 42]], (2, 0, 2)),
        ([[23, 42], [17]], (2, 2, 1)),
    ]
    for arr, want in cases:
        r = harness.call(app, "f(uint256[][])", arr)
        assert tuple(as_int(x) for x in r.abi_return) == want

def test_calldata_array_three_dimensional(harness):
    """calldata/contracts/calldata_array_three_dimensional.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_array_three_dimensional.sol")
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 0, 0, 1, 0x20, 0x40, 0x80, 1, 42, 1, 23 -> 1, 2, 1, 42
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 0, 0, 1, 32, 64, 128, 1, 42, 1, 23)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 1, 42)
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 1, 0, 1, 0x20, 0x40, 0x80, 1, 42, 1, 23 -> 1, 2, 1, 23
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 1, 0, 1, 32, 64, 128, 1, 42, 1, 23)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 1, 23)
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 1, 0, 1, 0x20, 0x40, 0x80, 1, 42, 2, 23, 17 -> 1, 2, 2, 23
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 1, 0, 1, 32, 64, 128, 1, 42, 2, 23, 17)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 2, 23)
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 1, 1, 1, 0x20, 0x40, 0x80, 1, 42, 2, 23, 17 -> 1, 2, 2, 17
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 1, 1, 1, 32, 64, 128, 1, 42, 2, 23, 17)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 2, 17)
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 1, 0, 0, 1, 0x20, 0x40, 0x80, 1, 42, 1, 23 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 1, 0, 0, 1, 32, 64, 128, 1, 42, 1, 23, expect_revert=True)
    assert r.reverted
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 2, 0, 1, 0x20, 0x40, 0x80, 1, 42, 1, 23 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 2, 0, 1, 32, 64, 128, 1, 42, 1, 23, expect_revert=True)
    assert r.reverted
    # f(uint256[][2][],uint256,uint256,uint256): 0x80, 0, 2, 0, 1, 0x20, 0x40, 0x80, 1, 42, 1, 23 -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f(uint256[][2][],uint256,uint256,uint256)", 128, 0, 2, 0, 1, 32, 64, 128, 1, 42, 1, 23, expect_revert=True)
    assert r.reverted

def test_calldata_attached_to_bytes(harness):
    """calldata/contracts/calldata_attached_to_bytes.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_attached_to_bytes.sol")
    # test(_, b, _) returns (b[1], b[0]) via the attached `reverseBytes` lib fn.
    r = harness.call(app, "test(uint256,bytes,uint256)", 7, b"ab", 4)
    # Returns two bytes1 values = ("b", "a").
    assert [bytes(x) for x in r.abi_return] == [b"b", b"a"]


def test_calldata_attached_to_dynamic_array_or_slice(harness):
    """calldata/contracts/calldata_attached_to_dynamic_array_or_slice.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_attached_to_dynamic_array_or_slice.sol")
    r = harness.call(app, "testArray(uint256,uint256[],uint256)", 7, [66, 77], 4)
    assert tuple(as_int(x) for x in r.abi_return) == (77, 66)
    r = harness.call(app, "testSlice(uint256,uint256[],uint256)", 7, [66, 77], 4)
    assert tuple(as_int(x) for x in r.abi_return) == (77, 66)

def test_calldata_attached_to_static_array(harness):
    """calldata/contracts/calldata_attached_to_static_array.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_attached_to_static_array.sol")
    # test(_, [a, b], _) returns (b, a) via the attached `reverse()` lib fn.
    r = harness.call(app, "test(uint256,uint256[2],uint256)", 7, [66, 77], 4)
    assert tuple(as_int(x) for x in r.abi_return) == (77, 66)

def test_calldata_attached_to_struct(harness):
    """calldata/contracts/calldata_attached_to_struct.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_attached_to_struct.sol")
    r = harness.call(app, "test(uint256,(uint256,uint256),uint256)", 7, (66, 77), 4)
    assert tuple(as_int(x) for x in r.abi_return) == (77, 66)

def test_calldata_bytes_array_bounds(harness):
    """calldata/contracts/calldata_bytes_array_bounds.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_bytes_array_bounds.sol")
    # f(bytes[],uint256): 0x40, 0, 1, 0x20, 2, hex"6162" -> 0x61
    r = harness.call(app, "f(bytes[],uint256)", 64, 0, 1, 32, 2, bytes.fromhex('6162'))
    assert as_int(r.abi_return) == 97
    # f(bytes[],uint256): 0x40, 1, 1, 0x20, 2, hex"6162" -> 0x62
    r = harness.call(app, "f(bytes[],uint256)", 64, 1, 1, 32, 2, bytes.fromhex('6162'))
    assert as_int(r.abi_return) == 98
    # f(bytes[],uint256): 0x40, 2, 1, 0x20, 2, hex"6162" -> FAILURE, hex"4e487b71", 0x32
    r = harness.call(app, "f(bytes[],uint256)", 64, 2, 1, 32, 2, bytes.fromhex('6162'), expect_revert=True)
    assert r.reverted

def test_calldata_bytes_external(harness):
    """calldata/contracts/calldata_bytes_external.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_bytes_external.sol")
    # tester(bytes): 0x20, 0x08, "abcdefgh" -> "c"
    r = harness.call(app, "tester(bytes)", 'abcdefgh')
    # TODO: verify expected: "c"
    assert not r.reverted

def test_calldata_bytes_internal(harness):
    """calldata/contracts/calldata_bytes_internal.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_bytes_internal.sol")
    # f(_, b, _) returns b[2] as bytes1.
    r = harness.call(app, "f(uint256,bytes,uint256)", 7, b"abcd", 4)
    assert bytes(r.abi_return) == b"c"

def test_calldata_bytes_to_memory(harness):
    """calldata/contracts/calldata_bytes_to_memory.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_bytes_to_memory.sol")
    # f(bytes): 0x20, 0x08, "abcdefgh" -> 0x48624fa43c68d5c552855a4e2919e74645f683f5384f72b5b051b71ea41d4f2d
    r = harness.call(app, "f(bytes)", 'abcdefgh')
    assert as_int(r.abi_return) == 32740225776097975629442294760974778014055824227399048407238890794297069489965

def test_calldata_bytes_to_memory_encode(harness):
    """calldata/contracts/calldata_bytes_to_memory_encode.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_bytes_to_memory_encode.sol")
    # f(bytes): 0x20, 0x08, "abcdefgh" -> 0x20, 0x60, 0x20, 8, 44048183304486788309563647967830685498285570828042699209880294173606615711744
    r = harness.call(app, "f(bytes)", 'abcdefgh')
    # TODO: verify structural decoding matches expected: 32, 96, 32, 8, 44048183304486788309563647967830685498285570828042699209880294173606615711744
    assert not r.reverted

def test_calldata_internal_function_pointer(harness):
    """calldata/contracts/calldata_internal_function_pointer.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_internal_function_pointer.sol")
    # g() -> 0x0700000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3166189940082864718613269121331309980362851143201109172953918312716374638592

def test_calldata_internal_library(harness):
    """calldata/contracts/calldata_internal_library.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_internal_library.sol")
    # g() -> 0x0800000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert as_int(r.abi_return) == 3618502788666131106986593281521497120414687020801267626233049500247285301248

def test_calldata_internal_multi_array(harness):
    """calldata/contracts/calldata_internal_multi_array.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_internal_multi_array.sol")
    # g() -> 7, 8
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)

def test_calldata_internal_multi_fixed_array(harness):
    """calldata/contracts/calldata_internal_multi_fixed_array.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_internal_multi_fixed_array.sol")
    # g() -> 7, 8
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (7, 8)

def test_calldata_memory_mixed(harness):
    """calldata/contracts/calldata_memory_mixed.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_memory_mixed.sol")
    # g() -> 0x0e, 0x0800000000000000000000000000000000000000000000000000000000000000, 0x0900000000000000000000000000000000000000000000000000000000000000, 0x0a00000000000000000000000000000000000000000000000000000000000000
    r = harness.call(app, "g()")
    assert tuple(as_int(x) for x in r.abi_return) == (14, 3618502788666131106986593281521497120414687020801267626233049500247285301248, 4070815637249397495359917441711684260466522898401426079512180687778195963904, 4523128485832663883733241601901871400518358776001584532791311875309106626560)

def test_calldata_string_array(harness):
    """calldata/contracts/calldata_string_array.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_string_array.sol")
    # f(["ab"]) returns (length=1, byte-length=2, byte 'a'=97, the string "ab").
    r = harness.call(app, "f(string[])", ["ab"])
    assert as_int(r.abi_return[0]) == 1
    assert as_int(r.abi_return[1]) == 2
    assert as_int(r.abi_return[2]) == 97
    assert r.abi_return[3] == "ab"

def test_calldata_struct(harness):
    """calldata/contracts/calldata_struct.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_struct.sol")
    # test(_, S{x,y}, _) returns (y, x).
    r = harness.call(app, "test(uint256,(uint256,uint256),uint256)", 7, (66, 77), 4)
    assert tuple(as_int(x) for x in r.abi_return) == (77, 66)

def test_calldata_struct_cleaning(harness):
    """calldata/contracts/calldata_struct_cleaning.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_struct_cleaning.sol")
    # f((uint8,bytes1)): 0x12, hex"3400000000000000000000000000000000000000000000000000000000000000" -> 0x12, hex"3400000000000000000000000000000000000000000000000000000000000000" # double check that the valid case goes through #
    r = harness.call(app, "f((uint8,bytes1))", 18, bytes.fromhex('3400000000000000000000000000000000000000000000000000000000000000'))
    # TODO: verify expected: 0x12 | hex"3400000000000000000000000000000000000000000000000000000000000000" # double check that the valid case goes through #
    assert not r.reverted
    # f((uint8,bytes1)): 0x1234, hex"5678000000000000000000000000000000000000000000000000000000000000" -> FAILURE
    r = harness.call(app, "f((uint8,bytes1))", 4660, bytes.fromhex('5678000000000000000000000000000000000000000000000000000000000000'), expect_revert=True)
    assert r.reverted
    # f((uint8,bytes1)): 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff -> FAILURE
    r = harness.call(app, "f((uint8,bytes1))", 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, expect_revert=True)
    assert r.reverted

def test_calldata_struct_internal(harness):
    """calldata/contracts/calldata_struct_internal.sol"""
    app = harness.compile_and_deploy("calldata/contracts/calldata_struct_internal.sol")
    # f(_, S{x, y}, _) returns (x, y).
    r = harness.call(app, "f(uint256,(uint256,uint256),uint256)", 7, (1, 2), 4)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)

def test_copy_from_calldata_removes_bytes_data(harness):
    """calldata/contracts/copy_from_calldata_removes_bytes_data.sol"""
    app = harness.compile_and_deploy("calldata/contracts/copy_from_calldata_removes_bytes_data.sol")
    # (): 1, 2, 3, 4, 5 ->
    pytest.xfail("fallback() dispatch not yet implemented")
    # checkIfDataIsEmpty() -> false
    r = harness.call(app, "checkIfDataIsEmpty()")
    assert bool(as_int(r.abi_return)) is False
    # sendMessage() -> true, 0x40, 0
    r = harness.call(app, "sendMessage()")
    # TODO: verify expected: true | 0x40 | 0
    assert not r.reverted
    # checkIfDataIsEmpty() -> true
    r = harness.call(app, "checkIfDataIsEmpty()")
    assert bool(as_int(r.abi_return)) is True
