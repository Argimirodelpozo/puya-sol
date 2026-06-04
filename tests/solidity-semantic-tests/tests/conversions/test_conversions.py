"""Tests for the conversions category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_signed_int, as_bytes,
)


def test_signed_narrowing_toint128(harness):
    """conversions/contracts/signed_narrowing.sol

    SafeCast-style signed narrowing int256 -> int128 must produce the canonical
    256-bit two's-complement so in-range negatives round-trip (downcasted==value);
    out-of-range still reverts. Regression guard for the V4 SafeCast.toInt128 path.
    """
    app = harness.compile_and_deploy("conversions/contracts/signed_narrowing.sol")
    for v in (-2987, -1, 0, 60, -(2 ** 127), 2 ** 127 - 1):
        assert as_signed_int(harness.call(app, "narrow(int256)", v).abi_return) == v
        assert as_signed_int(harness.call(app, "toI128(int256)", v).abi_return) == v
    # out of int128 range -> revert
    harness.call(app, "toI128(int256)", 2 ** 130, expect_revert=True)
    harness.call(app, "toI128(int256)", -(2 ** 130), expect_revert=True)


def test_asm_uintn_mask(harness):
    """conversions/contracts/asm_uintn_mask.sol

    A Yul `and(x, MASK)` yields a non-minimal biguint (AVM b& keeps the wider
    operand's byte width); returning it as uintN must not trip puya's
    biguint->arc4.uintN `len <= n/8` overflow assert. Guard for the width trim
    that unblocked the V4 swap uint160 sqrtPrice math.
    """
    app = harness.compile_and_deploy("conversions/contracts/asm_uintn_mask.sol")
    # bit set ABOVE the mask width is dropped; the low pattern survives
    assert as_int(harness.call(app, "mask160(uint256)", (1 << 200) | 0xDEADBEEF).abi_return) == 0xDEADBEEF
    assert as_int(harness.call(app, "mask128(uint256)", (1 << 200) | 0xCAFE).abi_return) == 0xCAFE
    # a full-width in-range value is preserved (no truncation of valid bits)
    assert as_int(harness.call(app, "mask160(uint256)", (1 << 160) - 1).abi_return) == (1 << 160) - 1


def test_signed_negate(harness):
    """conversions/contracts/signed_negate.sol

    Unary minus + uint256() cast on a NEGATIVE int256 — the V4
    SwapMath.computeSwapStep exact-in shape `uint256(-amountRemaining)`. Guards the
    256-bit two's-complement negation (must round-trip, not give 0 / a huge value).
    """
    app = harness.compile_and_deploy("conversions/contracts/signed_negate.sol")
    assert as_int(harness.call(app, "negToUint(int256)", -2000).abi_return) == 2000
    assert as_int(harness.call(app, "negToUint(int256)", -1).abi_return) == 1
    assert harness.call(app, "isNeg(int256)", -2000).abi_return is True
    assert harness.call(app, "isNeg(int256)", 5).abi_return is False
    # amountRemainingLessFee: uint256(-(-2000)) * (1e6-3000)/1e6 == 1994
    assert as_int(harness.call(app, "lessFee(int256,uint24)", -2000, 3000).abi_return) == 1994


def test_function_type_array_to_storage(harness):
    """conversions/contracts/function_type_array_to_storage.sol"""
    app = harness.compile_and_deploy("conversions/contracts/function_type_array_to_storage.sol")
    # testViewToDefault() -> 12, 22
    r = harness.call(app, "testViewToDefault()")
    assert tuple(as_int(x) for x in r.abi_return) == (12, 22)
    # testPureToDefault() -> 13, 23
    r = harness.call(app, "testPureToDefault()")
    assert tuple(as_int(x) for x in r.abi_return) == (13, 23)
    # testPureToView() -> 13, 23
    r = harness.call(app, "testPureToView()")
    assert tuple(as_int(x) for x in r.abi_return) == (13, 23)

def test_string_to_bytes(harness):
    """conversions/contracts/string_to_bytes.sol"""
    app = harness.compile_and_deploy("conversions/contracts/string_to_bytes.sol")
    # Reinterprets string as bytes — algosdk returns bytes (list[int]).
    r = harness.call(app, "f(string)", "Hello")
    assert bytes(r.abi_return) == b"Hello"
