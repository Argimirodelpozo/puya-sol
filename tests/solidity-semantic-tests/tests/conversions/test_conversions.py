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


@pytest.mark.xfail(
    reason="puya AWST->IR lowering drops a multi-return call destructured directly "
    "into struct fields (the materialised return is lost) — silent miscompile, "
    "reproduces at -O0/1/2. See puyabug.md / memory uros-multireturn-struct-destructure-dce. "
    "Backend fix owned elsewhere; V4 uses the local-var-destructure workaround.",
    strict=False,
)
def test_multireturn_into_struct_fields(harness):
    """conversions/contracts/multireturn_struct.sol

    Guard for the puya multi-return → struct-field lowering bug. test4(5) must == 30
    and test2(5) == 40; currently both return 0 because the call is dropped. Flips to
    xpass when puya is fixed (then un-xfail).
    """
    app = harness.compile_and_deploy("conversions/contracts/multireturn_struct.sol")
    assert as_int(harness.call(app, "test4(uint256)", 5).abi_return) == 30
    assert as_int(harness.call(app, "test2(uint256)", 5).abi_return) == 40


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


def test_signed_int128_neg(harness):
    """conversions/contracts/signed_int128_neg.sol

    The V4 SqrtPriceMath.getAmount0/1Delta(int128) NEGATIVE branch (remove
    liquidity). A signed `int128 x < 0` compare must hold for a sub-256 negative,
    and `uint128(-x)` must recover the magnitude — not ~2^128. Mirrors the add-path
    int24/int128 sign-extension fixes (#49/#50) on the remove side: a negative
    int128 held as a 128-bit two's complement (not sign-extended to 256-bit) would
    make `x < 0` false and `uint128(x)` ~2^128 (the observed garbage take amount).
    Covers three origins of the negative: ABI param, a subtraction, a state round-trip.
    """
    app = harness.compile_and_deploy("conversions/contracts/signed_int128_neg.sol")
    BIG = 1000000000000000000  # ~1e18, well inside int128 but > uint64 sign bit

    # ---- param-origin -------------------------------------------------------
    assert harness.call(app, "isNeg(int128)", -2000).abi_return is True
    assert harness.call(app, "isNeg(int128)", -BIG).abi_return is True
    assert harness.call(app, "isNeg(int128)", 2000).abi_return is False
    assert as_int(harness.call(app, "mag(int128)", -2000).abi_return) == 2000
    assert as_int(harness.call(app, "mag(int128)", -BIG).abi_return) == BIG
    assert as_int(harness.call(app, "mag(int128)", 2000).abi_return) == 2000

    # ---- computed-origin (subtraction) -------------------------------------
    assert harness.call(app, "isNegSub(int128,int128)", 100, 300).abi_return is True
    assert harness.call(app, "isNegSub(int128,int128)", 300, 100).abi_return is False
    assert as_int(harness.call(app, "magSub(int128,int128)", 100, 300).abi_return) == 200
    assert as_int(harness.call(app, "magSub(int128,int128)", 300, 100).abi_return) == 200

    # ---- round-trip-origin (state int128) ----------------------------------
    harness.call(app, "storeNeg(int128)", -BIG)
    assert harness.call(app, "isNegStored()").abi_return is True
    assert as_int(harness.call(app, "magStored()").abi_return) == BIG
    harness.call(app, "storeNeg(int128)", 4242)
    assert harness.call(app, "isNegStored()").abi_return is False
    assert as_int(harness.call(app, "magStored()").abi_return) == 4242


def test_signed_int128_neg_ternary(harness):
    """conversions/contracts/signed_int128_neg.sol — the getAmount*Delta(int128) ternary shape.

    A ternary RETURNING int256 with a signed `x < 0` condition, over a negative int128
    param. This is the exact shape of V4 SqrtPriceMath.getAmount0/1Delta(int128)'s
    negative branch (the remove-liquidity path). Fixed by sign-extending signed
    64<N<256 params to canonical 256-bit at the ARC4 decode (FunctionBuilder), so the
    `-x` negation and `x < 0` condition both read the sign correctly.
    """
    app = harness.compile_and_deploy("conversions/contracts/signed_int128_neg.sol")
    BIG = 1000000000000000000
    # branch returns int256; on the AVM it's an ARC4 uint256, so a NEGATIVE result is
    # the 256-bit two's complement — sign-interpret before comparing.
    def s256(u):
        return u - (1 << 256) if u >= (1 << 255) else u
    assert s256(as_int(harness.call(app, "branch(int128)", -2000).abi_return)) == 2000
    assert s256(as_int(harness.call(app, "branch(int128)", 2000).abi_return)) == -2000
    assert s256(as_int(harness.call(app, "branch(int128)", -BIG).abi_return)) == BIG


def test_bitnot_biguint(harness):
    """conversions/contracts/bitnot_biguint.sol

    Bitwise NOT (~) on a biguint-backed intN (N>64) must mask to the type width
    (~x = x XOR (2^N-1)), not invert the raw byte representation. Guards the V4
    LPFee `x & ~OVERRIDE_FEE_FLAG` pattern, which silently became a no-op before.
    """
    app = harness.compile_and_deploy("conversions/contracts/bitnot_biguint.sol")
    M128 = (1 << 128) - 1
    assert as_int(harness.call(app, "notU128(uint128)", 5).abi_return) == M128 - 5
    assert as_int(harness.call(app, "notU128(uint128)", 0).abi_return) == M128
    # clear bit 127 of a value that has it set + low bits: low bits survive
    assert as_int(harness.call(app, "clearTopBit(uint128)", (1 << 127) | 0xABCD).abi_return) == 0xABCD
    # clearing bit 127 when not set is a no-op
    assert as_int(harness.call(app, "clearTopBit(uint128)", 0xABCD).abi_return) == 0xABCD
    M160 = (1 << 160) - 1
    assert as_int(harness.call(app, "notU160(uint160)", 7).abi_return) == M160 - 7


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
