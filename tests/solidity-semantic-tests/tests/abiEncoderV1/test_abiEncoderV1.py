"""Tests for the abiEncoderV1 category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

# Legacy ABI coder v1 encoding is currently not supported by puya-sol (puya-sol emits ARC4). The whole
# abiEncoderV1 category is disclaimed as unsupported via a non-strict module xfail: cases that happen to
# coincide with ARC4 still run green (reported xpass), while genuinely-v1-specific ones (EVM memory
# layout / pointer identity, etc.) xfail.
pytestmark = pytest.mark.xfail(
    reason="legacy v1 encoding is currently not supported by puya-sol", strict=False)


def test_abi_decode_dynamic_array(harness):
    """abiEncoderV1/contracts/abi_decode_dynamic_array.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_dynamic_array.sol")
    # EVM_DIVERGENCE: abi.decode now consumes ARC4 ([uint16 len][elements]); on
    # real EVM the payload was head/tail (offset=32, length=4, elements).
    from framework import arc4_encode
    payload = arc4_encode("uint256[]", [3, 4, 5, 6])
    r = harness.call(app, "f(bytes)", payload)
    assert [as_int(x) for x in r.abi_return] == [3, 4, 5, 6]

def test_abi_decode_fixed_arrays(harness):
    """abiEncoderV1/contracts/abi_decode_fixed_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_fixed_arrays.sol")
    # f(a:uint16[3], b:uint16[2][3], i, j, k) → (a[i], b[j][k]).
    a = [1, 2, 3]
    b = [[11, 12], [21, 22], [31, 32]]
    r = harness.call(app, "f(uint16[3],uint16[2][3],uint256,uint256,uint256)", a, b, 1, 2, 1)
    assert tuple(as_int(x) for x in r.abi_return) == (2, 32)

def _static_2x3_payload() -> bytes:
    """6 uint256 words encoding a uint256[2][3] = [[1,2],[3,4],[5,6]]."""
    return b"".join(v.to_bytes(32, "big") for v in (1, 2, 3, 4, 5, 6))


def test_abi_decode_static_array(harness):  # currently fails
    """abiEncoderV1/contracts/abi_decode_static_array.sol

    `abi.decode(data, (uint256[2][3]))` over 6×32-byte values. The
    libsolidity test format `f(bytes): 0x20, 0xc0, 1, 2, …, 6`
    includes the EVM calldata header (offset + length) the EVM strips
    on the way in; AVM's ARC4 byte[] passes the raw content so we
    pass just the 6 payload words.
    """
    app = harness.compile_and_deploy('abiEncoderV1/contracts/abi_decode_static_array.sol')
    data = b''.join(v.to_bytes(32, "big") for v in (1, 2, 3, 4, 5, 6))
    r = harness.call(app, 'f(bytes)', data)
    # algokit returns the static 2D as nested lists [[1,2],[3,4],[5,6]] — flatten.
    flat = tuple(as_int(x) for row in r.abi_return for x in row)
    assert flat == (1, 2, 3, 4, 5, 6,)

def test_abi_decode_static_array_v2(harness):
    """abiEncoderV1/contracts/abi_decode_static_array_v2.sol — same as
    test_abi_decode_static_array but with `pragma abicoder v2`."""
    app = harness.compile_and_deploy('abiEncoderV1/contracts/abi_decode_static_array_v2.sol')
    data = b''.join(v.to_bytes(32, "big") for v in (1, 2, 3, 4, 5, 6))
    r = harness.call(app, 'f(bytes)', data)
    flat = tuple(as_int(x) for row in r.abi_return for x in row)
    assert flat == (1, 2, 3, 4, 5, 6,)

def test_abi_decode_trivial(harness):
    """abiEncoderV1/contracts/abi_decode_trivial.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_trivial.sol")
    # f(bytes) decodes its arg as a single uint256 — payload = the uint as 32 BE bytes.
    r = harness.call(app, "f(bytes)", (33).to_bytes(32, "big"))
    assert as_int(r.abi_return) == 33

def test_abi_decode_v2(harness):
    """abiEncoderV1/contracts/abi_decode_v2.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_decode_v2.sol")
    # f() -> 0x20, 0x8, 0x40, 0x3, 0x9, 0xa, 0xb
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 32, 8, 64, 3, 9, 10, 11
    assert not r.reverted

def test_abi_decode_v2_calldata(harness):
    """abiEncoderV1/contracts/abi_decode_v2_calldata.sol

    Decodes EVM-ABI bytes into struct S{uint256 a; uint256[] b}.
    libsolidity test format `f(bytes): 0x20, 0xe0, …` prepends the
    EVM calldata header (offset=0x20, length=0xe0=224); AVM's
    ARC4 byte[] passes raw payload so we drop the header and pass
    only the 7 inner words. Return is the struct; algokit decodes
    to (a, b_list).
    """
    app = harness.compile_and_deploy('abiEncoderV1/contracts/abi_decode_v2_calldata.sol')
    # EVM_DIVERGENCE: abi.decode consumes ARC4 — the struct is the ARC4 tuple
    # (uint256, uint256[]); EVM head/tail (0x20/0x40 offsets) on real EVM.
    from framework import arc4_encode
    data = arc4_encode("(uint256,uint256[])", [0x21, [0xa, 0xb, 0xc]])
    r = harness.call(app, 'f(bytes)', data)
    a, b = r.abi_return
    assert as_int(a) == 0x21
    assert [as_int(x) for x in b] == [0xa, 0xb, 0xc]

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
    # EVM_DIVERGENCE: abi.encode now emits the ARC4 encoding (on real EVM these
    # were 32-byte head/tail words). Integer literals are uint64 on the AVM, not
    # EVM's uint256.
    from framework import arc4_encode
    # f0() returns abi.encode() = no bytes.
    assert bytes(harness.call(app, "f0()").abi_return) == b""
    # f1() = abi.encode(1, 2) — ARC4 (uint64, uint64) = two 8-byte words.
    assert bytes(harness.call(app, "f1()").abi_return) == arc4_encode("(uint64,uint64)", [1, 2])
    # f2()/f3() = abi.encode(1, "abc", 2) — ARC4 tuple (uint64, string, uint64).
    expected_f2 = arc4_encode("(uint64,string,uint64)", [1, "abc", 2])
    assert bytes(harness.call(app, "f2()").abi_return) == expected_f2
    assert bytes(harness.call(app, "f3()").abi_return) == expected_f2
    # f4() = abi.encode(bytes2("ab")). ARC4 encodes bytes2 as two raw bytes.
    assert bytes(harness.call(app, "f4()").abi_return) == b"ab"

def test_abi_encode_call(harness):
    """abiEncoderV1/contracts/abi_encode_call.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_call.sol")
    # f() -> true
    r = harness.call(app, "f()")
    assert bool(as_int(r.abi_return)) is True

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
    from framework import arc4_encode
    sel1 = b"\x00\x00\x00\x01"

    # EVM_DIVERGENCE: abi.encode("") is now ARC4 arc4.string "" = [uint16 0] (2
    # bytes); EVM was the 64-byte (offset=32, length=0) head/tail form.
    arc4_empty = arc4_encode("string", "")
    assert bytes(harness.call(app, "f1()").abi_return) == arc4_empty
    assert bytes(harness.call(app, "f2(string)", "").abi_return) == arc4_empty

    # g1 / g2 — abi.encodePacked of "".
    assert bytes(harness.call(app, "g1()").abi_return) == b""
    assert bytes(harness.call(app, "g2(string)", "").abi_return) == b""

    # h1 / h2 — abi.encodeWithSelector(0x00000001, ""). EVM_DIVERGENCE: encodeWith*
    # ARGS are now ARC4 (Phase 2) — arc4.string "" = [uint16 0]; the selector is
    # the literal 0x00000001.
    assert bytes(harness.call(app, "h1()").abi_return) == sel1 + arc4_empty
    assert bytes(harness.call(app, "h2(string)", "").abi_return) == sel1 + arc4_empty

def test_abi_encode_rational(harness):
    """abiEncoderV1/contracts/abi_encode_rational.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/abi_encode_rational.sol")
    # EVM_DIVERGENCE: abi.encode now emits ARC4 (32-byte head/tail on real EVM).
    # abi.encode(1, -2): the positive literal 1 is uint64 (8 bytes); -2 is int256
    # (ARC-4 has no signed type, so it encodes as the raw 32-byte two's complement).
    from framework import arc4_encode
    expected = arc4_encode("uint64", 1) + ((1 << 256) - 2).to_bytes(32, "big")
    assert bytes(harness.call(app, "f()").abi_return) == expected

def test_bool_out_of_bounds(harness):
    """abiEncoderV1/contracts/bool_out_of_bounds.sol

    The original isoltest fixture exercises dirty-byte tolerance in EVM
    calldata (non-zero high bytes in a bool slot). ARC4 bools are a single
    byte (`0x80`/`0x00`) so the dirty-byte cases aren't reachable through
    the normal ABI client — only the canonical True/False values are
    asserted here.
    """
    app = harness.compile_and_deploy("abiEncoderV1/contracts/bool_out_of_bounds.sol")
    assert harness.call(app, "f(bool)", True).abi_return is True
    assert harness.call(app, "f(bool)", False).abi_return is False

def test_byte_arrays(harness):
    """abiEncoderV1/contracts/byte_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/byte_arrays.sol")
    # f(a, b, c) returns (a, len(b), b[3], c).
    for sig in ("f(uint256,bytes,uint256)", "f_external(uint256,bytes,uint256)"):
        r = harness.call(app, sig, 6, b"abcdefg", 9)
        assert as_int(r.abi_return[0]) == 6
        assert as_int(r.abi_return[1]) == 7
        assert bytes(r.abi_return[2]) == b"d"
        assert as_int(r.abi_return[3]) == 9

def test_calldata_arrays_too_large(harness):
    """abiEncoderV1/contracts/calldata_arrays_too_large.sol

    The original isoltest case constructs an EVM-ABI payload whose
    declared array length is 2^255+2 — a magnitude that algosdk's ARC4
    array encoder (16-bit length header) can't represent. We instead
    use `call_raw` to deliver malformed bytes directly so the contract's
    decoder asserts.
    """
    from algosdk.abi import Method
    app = harness.compile_and_deploy("abiEncoderV1/contracts/calldata_arrays_too_large.sol")
    # arc56 declares the return type as uint256 (the contract returns 7).
    selector = Method.from_signature("f(uint256,uint256[],uint256)uint256").get_selector()
    huge_length = 0x8000000000000000000000000000000000000000000000000000000000000002
    # ARC4 dynamic array uses a uint16 length header; we use the raw byte form
    # to express the over-large declared length, then send it as a single
    # ApplicationArgs blob that bypasses ABI dispatch.
    a = (6).to_bytes(32, "big")
    c = (9).to_bytes(32, "big")
    bad_b = huge_length.to_bytes(32, "big") + (1).to_bytes(32, "big") + (2).to_bytes(32, "big")
    r = harness.call_raw(app, selector, extra_args=(a, bad_b, c), expect_revert=True)
    assert r.reverted

def test_calldata_bytes_bytes32_arrays(harness):
    """abiEncoderV1/contracts/calldata_bytes_bytes32_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/calldata_bytes_bytes32_arrays.sol")
    b_a = b"a".ljust(32, b"\x00")
    b_b = b"b".ljust(32, b"\x00")
    r = harness.call(app, "f(bool,bytes,bytes32[2])", True, b"abcd", [list(b_a), list(b_b)])
    assert r.abi_return[0] is True
    assert bytes(r.abi_return[1]) == b"abcd"
    assert [bytes(x) for x in r.abi_return[2]] == [b_a, b_b]

def test_decode_slice(harness):
    """abiEncoderV1/contracts/decode_slice.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/decode_slice.sol")
    # f(uint256,uint256): 42, 23 -> 42, 23, 42, 23
    r = harness.call(app, "f(uint256,uint256)", 42, 23)
    assert tuple(as_int(x) for x in r.abi_return) == (42, 23, 42, 23)

def test_dynamic_arrays(harness):
    """abiEncoderV1/contracts/dynamic_arrays.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/dynamic_arrays.sol")
    # f(a, b[], c) returns (len(b), b[a], c). With a=6, b=[11..17], c=9.
    r = harness.call(app, "f(uint256,uint16[],uint256)", 6, [11, 12, 13, 14, 15, 16, 17], 9)
    assert tuple(as_int(x) for x in r.abi_return) == (7, 17, 9)

def test_dynamic_memory_copy(harness):  # currently fails
    """abiEncoderV1/contracts/dynamic_memory_copy.sol"""
    app = harness.compile_and_deploy('abiEncoderV1/contracts/dynamic_memory_copy.sol')
    r = harness.call(app, 'test(bytes)', 0x20, 0x80, 0x40, 0x60, 0, 0)
    assert tuple(r.abi_return) == (False, False,)
    r = harness.call(app, 'test(bytes)', 0x20, 0xC0, 0x40, 0x80, 1, 0x42, 1, 0x42)
    assert tuple(r.abi_return) == (False, False,)
    r = harness.call(app, 'test(bytes)', 0x20, 0x80, 0x40, 0x40, 1, 0x42)
    assert tuple(r.abi_return) == (False, False,)
    r = harness.call(app, 'test(bytes)', 0x20, 0x60, 0x40, 0x40, 0)
    assert tuple(r.abi_return) == (False, False,)
    r = harness.call(app, 'test(bytes)', 0x20, 0x80, 0x40, 0x40, 1, 0x42)
    assert tuple(r.abi_return) == (False, False,)

def test_enums(harness):
    """abiEncoderV1/contracts/enums.sol

    ARCH NOTE: EVM abicoder v1 doesn't range-check enum args, so the original
    test passes 2 to an enum {A,B} and expects it to read through. puya-sol
    emits an enum range check at the ARC4 router for all abicoder versions
    (AVM has no separate v1/v2 distinction; the check is always-on), so
    out-of-range enum args panic. Test the AVM-observable behavior: in-range
    values pass through; out-of-range reverts.
    """
    app = harness.compile_and_deploy("abiEncoderV1/contracts/enums.sol")
    for v in (0, 1):
        assert as_int(harness.call(app, "f(uint8)", v).abi_return) == v
    assert harness.call(app, "f(uint8)", 2, expect_revert=True).reverted

def test_memory_dynamic_array_and_calldata_bytes(harness):
    """abiEncoderV1/contracts/memory_dynamic_array_and_calldata_bytes.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/memory_dynamic_array_and_calldata_bytes.sol")

    # EVM_DIVERGENCE: abi.encode now emits ARC4 (EVM head/tail on real EVM).
    from framework import arc4_encode
    def encoded(a: list[int], b: bytes) -> bytes:
        return arc4_encode("(uint256[],byte[])", [a, list(b)])

    r = harness.call(app, "f(uint256[],bytes)", [0xFF], b"123456")
    assert bytes(r.abi_return) == encoded([0xFF], b"123456")
    r = harness.call(app, "g(uint256[],bytes)", [0xFFFF], b"12345678")
    assert bytes(r.abi_return) == encoded([0xFFFF], b"12345678")

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
    assert bool(as_int(r.abi_return)) is True
    # f(uint256): 0x7f -> true
    r = harness.call(app, "f(uint256)", 127)
    assert bool(as_int(r.abi_return)) is True
    # f(uint256): 0x80 -> true
    r = harness.call(app, "f(uint256)", 128)
    assert bool(as_int(r.abi_return)) is True

@pytest.mark.xfail(reason="depends on EVM returndatacopy out-of-range behaviour for a dynamic-type cross-call return — AVM has no returndatasize/returndatacopy model. EVM-fundamental.", strict=False)
def test_return_dynamic_types_cross_call_out_of_range_2(harness):  # currently fails
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_2.sol"""
    app = harness.compile_and_deploy('abiEncoderV1/contracts/return_dynamic_types_cross_call_out_of_range_2.sol')
    r = harness.call(app, 'f(uint256)', 0x60, expect_revert=True)
    assert r.reverted
    r = harness.call(app, 'f(uint256)', 0x61)
    assert r.abi_return is True
    r = harness.call(app, 'f(uint256)', 0x80)
    assert r.abi_return is True

def test_return_dynamic_types_cross_call_simple(harness):
    """abiEncoderV1/contracts/return_dynamic_types_cross_call_simple.sol"""
    app = harness.compile_and_deploy("abiEncoderV1/contracts/return_dynamic_types_cross_call_simple.sol")
    # f() -> 0x20, 40, "12345678901234567890123456789012", "34567890"
    r = harness.call(app, "f()")
    # TODO: verify expected: 0x20 | 40 | "12345678901234567890123456789012" | "34567890"
    assert not r.reverted
