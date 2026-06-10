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


def test_multireturn_into_struct_fields(harness):
    """conversions/contracts/multireturn_struct.sol

    Regression guard for the puya multi-return -> struct-field lowering bug
    (the call used to be DCE-dropped, so both returned 0). Now fixed; promoted
    from xfail to guard against the drop re-appearing for this shape.
    test4(5) == 30 and test2(5) == 40.
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


def test_safecast_toint128(harness):
    """conversions/contracts/safecast_toint128.sol

    SafeCast.toInt128(int256) incl. the MIN_INT128 (-2^127) edge: must round-trip
    (downcasted == value), and out-of-range must revert. Guards signed int128(int256)
    narrowing + the boundary check (V4/OZ SafeCast). int128 returns surface as the
    unsigned 256-bit two's-complement bit-pattern -> s256.
    """
    app = harness.compile_and_deploy("conversions/contracts/safecast_toint128.sol")
    MIN, MAX = -(1 << 127), (1 << 127) - 1

    def s256(u):
        return u - (1 << 256) if u >= (1 << 255) else u

    def f(v):
        return s256(as_int(harness.call(app, "toInt128(int256)", v).abi_return))

    assert f(MIN) == MIN          # MIN_INT128 round-trips (no spurious revert)
    assert f(MAX) == MAX
    assert f(-5) == -5 and f(1000) == 1000
    assert harness.call(app, "toInt128(int256)", MIN - 1, expect_revert=True).reverted
    assert harness.call(app, "toInt128(int256)", MAX + 1, expect_revert=True).reverted


def test_int256_mul_large(harness):
    """conversions/contracts/int256_mul_large.sol

    The V4 TickMath.getTickAtSqrtPrice log2 step `log_2 * 255738958999603826347141`
    with a large NEGATIVE int256 log_2 (the xfail blamed this mul for exceeding the
    biguint range). Signed int256 mul must wrap mod 2^256 to the right two's complement
    without spuriously reverting. Result interpreted as 256-bit signed.
    """
    app = harness.compile_and_deploy("conversions/contracts/int256_mul_large.sol")
    C = 255738958999603826347141

    def s256(u):
        return u - (1 << 256) if u >= (1 << 255) else u

    def f(a):
        return s256(as_int(harness.call(app, "f(int256)", a).abi_return))

    for a in [-64 * (1 << 64), -(1 << 70), (1 << 70), -100, 100, 0]:
        assert f(a) == a * C, f"f({a}) = {f(a)}, expected {a * C}"


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



def test_int128_array_element_signextend(harness):
    """conversions/contracts/int128_arr_check.sol

    CUSTOM regression guard (NOT vendored from the upstream Solidity semantic
    suite). A signed sub-256 array element (int128) is decoded as its raw N-bit
    two's complement and must be sign-extended to the canonical 256-bit form on
    read, so `a[i]` compares/arithmetics equal to a sign-extended scalar of the
    same value. Before the fix, eq0([-777], -777) was False (element 2^128-777
    vs scalar 2^256-777).
    """
    app = harness.compile_and_deploy("conversions/contracts/int128_arr_check.sol")
    # scalar + element-return round-trips (sanity — unaffected by the fix)
    assert as_signed_int(harness.call(app, "ident(int128)", -777).abi_return) == -777
    assert as_signed_int(harness.call(app, "get0(int128[])", [-777]).abi_return) == -777
    # the core fix: a negative element compares equal to the negative scalar
    assert harness.call(app, "eq0(int128[],int128)", [-777], -777).abi_return is True
    # a positive value still compares equal (sign-extension is a no-op there)
    assert harness.call(app, "eq0(int128[],int128)", [777], 777).abi_return is True
    # a genuine mismatch is still reported False (guards against over-eager extend)
    assert harness.call(app, "eq0(int128[],int128)", [-777], -778).abi_return is False
    # sign test: negative element is < 0, positive is not
    assert harness.call(app, "gt0(int128[])", [-777]).abi_return is True
    assert harness.call(app, "gt0(int128[])", [777]).abi_return is False
    # arithmetic across two decoded signed elements (widened to int256)
    assert as_signed_int(harness.call(app, "sum2(int128[])", [-777, 1000]).abi_return) == 223
    assert as_signed_int(harness.call(app, "sum2(int128[])", [-5, -10]).abi_return) == -15
    # memory-array element access path (true iff t == -5)
    assert harness.call(app, "eqMem(int128)", -5).abi_return is True
    assert harness.call(app, "eqMem(int128)", 7).abi_return is False


def test_sol_eb_bug_guards(harness):
    """conversions/contracts/sol_eb_bug_guards.sol

    CUSTOM regression guard (NOT vendored) for four sol-eb/ builder-audit bugs:
    (A) signed compound `-=` reverted; (B) signed `>>` was a logical shift;
    (C) `bool` in abi.encode was 8 bytes not 32; (D) enum `==` double-evaluated
    a side-effecting operand.
    """
    app = harness.compile_and_deploy("conversions/contracts/sol_eb_bug_guards.sol")
    # A — signed compound subtraction, all widths + the int8 INT_MIN boundary
    assert as_signed_int(harness.call(app, "cSub8(int8,int8)", 1, 2).abi_return) == -1
    assert as_signed_int(harness.call(app, "cSub8(int8,int8)", -100, 28).abi_return) == -128
    assert as_signed_int(harness.call(app, "cSub128(int128,int128)", 5, 20).abi_return) == -15
    assert as_signed_int(harness.call(app, "cSub256(int256,int256)", -1, -1).abi_return) == 0
    # B — arithmetic shift right (negative sign-fills; positive zero-fills; >=256 saturates)
    assert as_signed_int(harness.call(app, "sar(int256,uint256)", -8, 1).abi_return) == -4
    assert as_signed_int(harness.call(app, "sar(int256,uint256)", -1, 5).abi_return) == -1
    assert as_signed_int(harness.call(app, "sar(int256,uint256)", 8, 1).abi_return) == 4
    assert as_signed_int(harness.call(app, "sar(int256,uint256)", -8, 300).abi_return) == -1
    assert as_signed_int(harness.call(app, "sar(int256,uint256)", 8, 300).abi_return) == 0
    # C — bool occupies a full 32-byte ABI word ending in 0x01
    b = bytes(harness.call(app, "encBool()").abi_return)
    assert len(b) == 96
    assert b[32:64] == bytes(31) + bytes([1])
    # D — the side-effecting operand evaluates exactly once
    assert harness.call(app, "enumOneEval()").abi_return is True
