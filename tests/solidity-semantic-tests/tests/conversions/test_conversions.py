"""Tests for the conversions category."""
import pytest
from eth_abi import encode as evm_encode

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
    # C — REMOVED: asserted bool occupies a full 32-byte EVM ABI word. abi.encode
    # is now ARC4 (arc4.bool is a single 0x80 byte), so the EVM word layout is moot.
    # D — the side-effecting operand evaluates exactly once
    assert harness.call(app, "enumOneEval()").abi_return is True


def test_struct_int128_field_signextend(harness):
    """conversions/contracts/struct_int128_field_signext.sol

    CUSTOM regression guard (NOT vendored). A signed int128 STRUCT FIELD must be
    sign-extended to canonical 256-bit on read. The sub-64-bit field case was
    already handled; the 64<N<256 case was not (eq(-5) returned false). Same
    class as the int128[] array-element and transient read fixes.
    """
    app = harness.compile_and_deploy("conversions/contracts/struct_int128_field_signext.sol")
    assert harness.call(app, "eq(int128)", -5).abi_return is True
    assert harness.call(app, "eq(int128)", 777).abi_return is True
    assert harness.call(app, "eq(int128)", -(2 ** 126)).abi_return is True
    assert as_signed_int(harness.call(app, "widen(int128)", -5).abi_return) == -5
    assert as_signed_int(harness.call(app, "widen(int128)", -(2 ** 126)).abi_return) == -(2 ** 126)
    assert as_signed_int(harness.call(app, "arith(int128)", -5).abi_return) == -4
    assert harness.call(app, "unsignedOk(uint128)", 2 ** 100).abi_return is True


def test_int128_every_read_surface(harness):
    """conversions/contracts/int128_everywhere.sol

    CUSTOM regression battery (NOT vendored). A signed sub-256 value (int128 /
    int24) must round-trip as canonical negative through EVERY read surface:
    state var, public auto-getter path, mapping value, dynamic + fixed array
    elements, struct field, struct-in-mapping field, abi.decode round-trip,
    calldata param, internal tuple return, comparisons on every container, and
    ternary reads. Closes the class behind the three historical fixes
    (int128[] elements, transients, struct fields) — checked at -5, -(2^126),
    and +777.
    """
    app = harness.compile_and_deploy("conversions/contracts/int128_everywhere.sol")
    for v in (-5, -(2 ** 126), 777):
        harness.call(app, "setAll(int128)", v)
        small = v if -(2 ** 23) <= v < 2 ** 23 else int.from_bytes(
            (v % 2 ** 24).to_bytes(3, "big"), "big", signed=True)
        for fn, exp in [("rState()", v), ("rMap()", v), ("rArr()", v), ("rFarr()", v),
                        ("rStructField()", v), ("rStructInMap()", v), ("rSmall()", small)]:
            assert as_signed_int(harness.call(app, fn).abi_return) == exp, (v, fn)
        assert as_signed_int(harness.call(app, "rDecode(int128)", v).abi_return) == v
        assert as_signed_int(harness.call(app, "rParam(int128)", v).abi_return) == v
        r = harness.call(app, "rTuple(int128)", v).abi_return
        assert (as_signed_int(r[0]), as_int(r[1])) == (v, 3)
        assert bool(harness.call(app, "cmpAll(int128)", v).abi_return) is True
        assert as_signed_int(harness.call(app, "rTernary(bool)", True).abi_return) == v


def test_struct_field_call_shapes(harness):
    """conversions/contracts/int128_everywhere.sol

    CUSTOM regression guard (NOT vendored). The two historical
    multireturn/struct-field call shapes evaluate exactly once and write:
    `(p.a, p.b) = mk2()` (the old silent-drop, fixed frontend-side via
    _emitAsStatement in 487de85f11 — the puya-DCE attribution was retracted)
    and `p.a = mk1(p.a)` (the old under-uros duplication shape; clean in
    plain context).
    """
    app = harness.compile_and_deploy("conversions/contracts/int128_everywhere.sol")
    assert tuple(as_int(x) for x in harness.call(app, "destructure()").abi_return) == (11, 22, 1)
    assert tuple(as_int(x) for x in harness.call(app, "fieldCall()").abi_return) == (6, 1)


def test_struct_param_selector(harness):
    """conversions/contracts/struct_param_selector.sol  (CUSTOM)

    A method selector for struct/array/enum params must hash the ARC4
    TUPLE expansion the callee router dispatches on — `(uint256,uint256)`,
    not `struct P`. These strings are puya's exact `method "..."` output;
    f.selector goes through buildMethodSelector, so a mismatch here means
    cross-contract abi.encodeCall/.call to the method silently misses
    dispatch. EVM uses keccak; ours is sha512_256 (EVM_DIVERGENCE)."""
    from framework import arc4_selector
    app = harness.compile_and_deploy("conversions/contracts/struct_param_selector.sol")
    assert bytes(harness.call(app, "selStruct()").abi_return) == \
        arc4_selector("takeStruct((uint256,uint256))uint256")
    assert bytes(harness.call(app, "selNested()").abi_return) == \
        arc4_selector("takeNested(((uint256,uint256),uint8))uint256")
    assert bytes(harness.call(app, "selStructArr()").abi_return) == \
        arc4_selector("takeStructArr((uint256,uint256)[])uint256")
    assert bytes(harness.call(app, "selEnumStruct()").abi_return) == \
        arc4_selector("takeEnumStruct((uint8,int8,byte[3]))uint256")


def test_getter_abi_validation(harness):
    """conversions/contracts/getter_abi_validation.sol  (CUSTOM)

    A public getter for `mapping(uint8 => V)` decodes its key as a full
    uint64 (selector "m(uint64)"), so a raw caller can pass an out-of-range
    key. The getter must apply the same buildABIEntryChecks a real method
    does — assert key <= 255 (abicoder v2) before hashing the slot — not
    silently read m[0]. Regression for the getter ABI-validation gap."""
    app = harness.compile_and_deploy("conversions/contracts/getter_abi_validation.sol")
    # in-range keys read the right slots
    assert as_int(harness.call(app, "m(uint64)", 0).abi_return) == 99
    assert as_int(harness.call(app, "m(uint64)", 5).abi_return) == 42
    # out-of-range key (256 > uint8 max) must REVERT, not read m[256 & 0xff]=m[0]
    r = harness.call(app, "m(uint64)", 256, expect_revert=True)
    assert r.reverted


def test_encodepacked_widths(harness):
    """conversions/contracts/encodepacked_widths.sol  (CUSTOM)

    abi.encodePacked uses EVM packed widths. Enum was the bug: packed as the
    8-byte native word instead of its 1-byte uint8 encoding — corrupting
    keccak256(abi.encodePacked(...)) and shifting every following argument.
    Addresses are explicitly adapted from the internal 32-byte account to the
    canonical low 20-byte Solidity address at this ABI boundary."""
    app = harness.compile_and_deploy("conversions/contracts/encodepacked_widths.sol")
    assert bytes(harness.call(app, "pEnum(uint8)", 1).abi_return) == b"\x01"
    assert bytes(harness.call(app, "pMix(uint8,uint8)", 5, 1).abi_return) == b"\x05\x01"
    assert bytes(harness.call(app, "pI8neg()").abi_return) == b"\xfd"
    assert bytes(harness.call(app, "pI128neg()").abi_return) == bytes.fromhex("ff" * 15 + "fd")
    from algosdk import account, encoding
    _, addr = account.generate_account()
    r = bytes(harness.call(app, "pAddr(address)", addr).abi_return)
    assert len(r) == 20 and r == encoding.decode_address(addr)[-20:]


def test_abi_decode_mixed_struct(harness):
    """conversions/contracts/abi_roundtrip_mixed.sol  (CUSTOM)

    abi.decode of a struct with mixed-width fields must field-walk (each
    field at its own 32-byte EVM slot), not slab-reinterpret the whole
    thing — the slab shortcut only works when ARC4 size == EVM size (all
    32-byte fields). With an int128 field (16 ARC4 bytes / 32 EVM bytes)
    the reinterpret mis-read every field from there on (b=-1, c wrong).
    The decode counterpart to the signed-aggregate encode fix; together
    they make abi.decode(abi.encode(P)) round-trip."""
    from algosdk import account, encoding
    asint = as_int
    def signed(x, bits=256):
        v = asint(x); return v - (1 << bits) if v >= (1 << (bits - 1)) else v
    app = harness.compile_and_deploy("conversions/contracts/abi_roundtrip_mixed.sol")
    a = account.generate_account()[1]
    rr = harness.call(app, "rt((uint256,int128,address,bool))", [42, -7, a, True]).abi_return
    assert asint(rr[0]) == 42
    assert signed(rr[1]) == -7
    expected_evm_address = encoding.encode_address(
        bytes(12) + encoding.decode_address(a)[-20:])
    assert rr[2] == expected_evm_address
    assert rr[3] is True
    rs = harness.call(app, "rtSmall((int128,uint8,bool))", [-9, 200, True]).abi_return
    assert signed(rs[0]) == -9
    assert asint(rs[1]) == 200
    assert rs[2] is True


def test_abi_static_array_subword_decode(harness):
    """conversions/contracts/abi_static_arr.sol  (CUSTOM)

    abi.decode of a static array with sub-32-byte elements (uint128[3]) must
    field-walk each 32-byte EVM slot, not slab-reinterpret the ARC4-packed
    bytes (which read 48 of 96 bytes → garbage, e.g. [0,11,0]). Decode
    counterpart to the static-array element ENCODE widening. (Signed
    static-array ENCODE sign-extension is a separate multi-path follow-up.)"""
    app = harness.compile_and_deploy("conversions/contracts/abi_static_arr.sol")
    r = harness.call(app, "rtU128()").abi_return
    assert tuple(as_int(x) for x in r) == (11, 22, 33), r
    assert len(bytes(harness.call(app, "encU128()").abi_return)) == 96


def test_abi_encode_signed_roundtrip(harness):
    """conversions/contracts/abi_static_arr.sol  (CUSTOM)
    Signed static array survives canonical abi.encode -> abi.decode."""
    app = harness.compile_and_deploy("conversions/contracts/abi_static_arr.sol")
    rt = harness.call(app, "rtI128()").abi_return
    assert tuple(as_signed_int(x) for x in rt) == (-7, 5, -1), rt


def test_abi_decode_nested_dynamic_arrays(harness):
    """conversions/contracts/abi_nested_dyn_decode.sol  (CUSTOM)

    abi.decode of a dynamic array whose ELEMENTS are themselves dynamic
    (uint256[][], uint256[][][], string[], bytes[]) — a recursive EVM
    offset-table walk that rebuilds the ARC4 layout. Previously a fail-loud
    hard-error. uint-element nesting round-trips through abi.encode (supported);
    string[]/bytes[] decode a real eth_abi-encoded blob (their abi.ENCODE is a
    separate pre-existing puya-backend limitation)."""
    app = harness.compile_and_deploy("conversions/contracts/abi_nested_dyn_decode.sol")
    # uint256[][] round-trip
    r = harness.call(app, "rtU256_2d()").abi_return
    assert tuple(as_int(x) for x in r) == (2, 11, 22, 33), r
    # uint256[][][] round-trip (recursion)
    r3 = harness.call(app, "rtU256_3d()").abi_return
    assert tuple(as_int(x) for x in r3) == (2, 8, 2, 10), r3
    # empty inner array edge case
    re = harness.call(app, "rtEmptyInner()").abi_return
    assert tuple(as_int(x) for x in re) == (2, 0, 3), re
    # string[] decode from an independently encoded canonical blob
    s = harness.call(
        app, "decStrArr(byte[])",
        list(evm_encode(["string[]"], [["hi", "abc"]]))).abi_return
    assert s[0] == "hi" and s[1] == "abc" and as_int(s[2]) == 2, s
    blob = evm_encode(["bytes[]"], [[b"\xaa\xbb", b"\xcc\xdd\xee"]])
    b = harness.call(app, "decBytesArr(byte[])", list(blob)).abi_return
    assert as_int(b[0]) == 2 and as_int(b[1]) == 2 and as_int(b[2]) == 3, b
    assert bytes(b[3]) == b"\xcc", b  # d[1][0]
    # S[] : array of dynamic structs (struct{uint256,string}) — full round-trip
    rs = harness.call(app, "rtStructArr()").abi_return
    assert as_int(rs[0]) == 42 and rs[1] == "hi" and as_int(rs[2]) == 7 \
        and rs[3] == "world!!" and as_int(rs[4]) == 2, rs
    # S[] decode cross-checked against an independent canonical blob
    sblob = evm_encode(
        ["(uint256,string)[]"], [[(99, "x"), (5, "yz"), (1, "")]])
    ds = harness.call(app, "decStructArr(byte[])", list(sblob)).abi_return
    assert as_int(ds[0]) == 99 and as_int(ds[1]) == 5 and as_int(ds[2]) == 3, ds


def _evm_word(value):
    return int(value).to_bytes(32, "big")


def _evm_bytes_payload(value):
    value = bytes(value)
    return _evm_word(len(value)) + value.ljust((len(value) + 31) // 32 * 32, b"\0")


def _evm_scalar_array_payload(values):
    return _evm_word(len(values)) + b"".join(_evm_word(v) for v in values)


def _evm_dynamic_children_payload(children):
    """EVM T[] payload where T is dynamically encoded."""
    head_size = 32 * len(children)
    offset = head_size
    heads = []
    for child in children:
        heads.append(_evm_word(offset))
        offset += len(child)
    return _evm_word(len(children)) + b"".join(heads) + b"".join(children)


def _evm_fixed_dynamic_children_payload(children):
    """EVM T[N] payload where T is dynamically encoded (no length word)."""
    offset = 32 * len(children)
    heads = []
    for child in children:
        heads.append(_evm_word(offset))
        offset += len(child)
    return b"".join(heads) + b"".join(children)


def _evm_single_dynamic(payload):
    return _evm_word(32) + payload


def test_external_evm_abi_decode_recurses(harness):
    """Canonical EVM head/tail input is transcoded recursively to ARC4.

    This covers narrow elements, fixed-of-dynamic arrays, three dynamic ranks,
    bytes/string elements, and dynamic structs. None of these payloads comes
    from puya-sol's encoder; the words are assembled independently below.
    """
    app = harness.compile_and_deploy("conversions/contracts/abi_nested_dyn_decode.sol")

    u8 = _evm_single_dynamic(_evm_scalar_array_payload([1, 200, 255]))
    got = harness.call(app, "decEvmU8(byte[])", list(u8)).abi_return
    assert tuple(as_int(x) for x in got) == (3, 1, 200, 255), got

    mixed = _evm_single_dynamic(_evm_fixed_dynamic_children_payload([
        _evm_scalar_array_payload([1, 513]),
        _evm_scalar_array_payload([9]),
    ]))
    got = harness.call(app, "decEvmMixed(byte[])", list(mixed)).abi_return
    assert tuple(as_int(x) for x in got) == (2, 513, 1, 9), got

    level2_a = _evm_dynamic_children_payload([
        _evm_scalar_array_payload([7, 8]),
    ])
    level2_b = _evm_dynamic_children_payload([
        _evm_scalar_array_payload([9]),
        _evm_scalar_array_payload([10]),
    ])
    three_d = _evm_single_dynamic(
        _evm_dynamic_children_payload([level2_a, level2_b]))
    got = harness.call(app, "decEvm3d(byte[])", list(three_d)).abi_return
    assert tuple(as_int(x) for x in got) == (2, 8, 2, 10), got

    strings = _evm_single_dynamic(_evm_dynamic_children_payload([
        _evm_bytes_payload(b"hi"), _evm_bytes_payload(b"abc"),
    ]))
    got = harness.call(app, "decStrArr(byte[])", list(strings)).abi_return
    assert got[0] == "hi" and got[1] == "abc" and as_int(got[2]) == 2, got

    struct_children = []
    for number, string in ((99, b"x"), (5, b"yz"), (1, b"")):
        struct_children.append(
            _evm_word(number) + _evm_word(64) + _evm_bytes_payload(string))
    structs = _evm_single_dynamic(_evm_dynamic_children_payload(struct_children))
    got = harness.call(app, "decStructArr(byte[])", list(structs)).abi_return
    assert tuple(as_int(x) for x in got) == (99, 5, 3), got


def test_abi_encode_dynamic_element_arrays(harness):
    """conversions/contracts/abi_nested_dyn_decode.sol  (CUSTOM)

    abi.encode(string[]) / abi.encode(bytes[]) — arrays whose elements are
    dynamic byte-arrays. encodeFromArc4Bytes builds each element's EVM tail
    directly from its ARC4 [uint16 len][body] bytes (reinterpreting raw bytes to
    a dynamic ARC4 byte-array type is rejected by puya). Validated byte-exact: a
    decode->re-encode must reproduce the original eth_abi blob. (Building such an
    array via literal element assignment is a separate open codegen gap, #22.)"""
    app = harness.compile_and_deploy("conversions/contracts/abi_nested_dyn_decode.sol")
    blob = evm_encode(["string[]"], [["hi", "abc", "Z"]])
    assert bytes(harness.call(app, "reencStrArr(byte[])", list(blob)).abi_return) == blob
    blob2 = evm_encode(["bytes[]"], [[b"\xaa\xbb", b"\xcc\xdd\xee"]])
    assert bytes(harness.call(app, "reencBytesArr(byte[])", list(blob2)).abi_return) == blob2
    # full literal-built round-trip: element assignment -> encode -> decode -> read
    s = harness.call(app, "rtStrArr()").abi_return
    assert s[0] == "foo" and s[1] == "barbaz" and as_int(s[2]) == 2, s
    b = harness.call(app, "rtBytesArr()").abi_return
    assert as_int(b[0]) == 2 and as_int(b[1]) == 3 and bytes(b[2]) == b"\xee", b


def test_explicit_conversion_matrix(harness):
    """conversions/contracts/explicit_conversion_matrix.sol — NOT an o.g.
    semantic test; coverage lane for calls/SolTypeConversion.cpp (enum casts,
    int<->bytesN at every carrier tier, narrowing masks, address/contract
    casts, bytesN resize)."""
    app = harness.compile_and_deploy(
        "conversions/contracts/explicit_conversion_matrix.sol")
    P = 1 << 256

    assert as_int(harness.call(
        app, "enumRoundTrip(uint8)", 2).abi_return) == 2
    r = harness.call(app, "enumRoundTrip(uint8)", 9, expect_revert=True)
    assert r.reverted  # out-of-range uint -> enum panics

    b1, b4, b8, b16, b32 = harness.call(app, "intToBytesTiers()").abi_return
    assert bytes(b1) == bytes.fromhex("ab")
    assert bytes(b4) == bytes.fromhex("deadbeef")
    assert bytes(b8) == bytes.fromhex("1122334455667788")
    assert bytes(b16) == (2 ** 100 + 7).to_bytes(16, "big")
    assert bytes(b32) == (2 ** 200 + 9).to_bytes(32, "big")

    a, b = harness.call(app, "bytesToIntTiers(bytes4,bytes32)",
                        bytes.fromhex("deadbeef"),
                        (2 ** 200 + 9).to_bytes(32, "big")).abi_return
    assert (as_int(a), as_int(b)) == (0xDEADBEEF, 2 ** 200 + 9)

    wide = 2 ** 220 + 0xFFEE_DDCC_BBAA_9988_7766
    u8, u32, u64, u128 = (as_int(x) for x in harness.call(
        app, "narrowingMasks(uint256)", wide).abi_return)
    assert (u8, u32, u64, u128) == (
        wide & 0xFF, wide & 0xFFFFFFFF,
        wide & (2 ** 64 - 1), wide & (2 ** 128 - 1))

    def signed(v):
        return v - P if v > P // 2 else v
    n, w = harness.call(
        app, "signedNarrowWiden(int256)", -300).abi_return
    # int8(-300) truncates to the low byte: -300 & 0xFF = 0xD4 = -44 (signed
    # returns travel as 256-bit two's complement on the wire).
    assert signed(as_int(n)) == -44
    assert signed(as_int(w)) == -44

    from algosdk.encoding import decode_address
    acct = harness.localnet.account.address
    as_i, back, self_addr = harness.call(
        app, "addressCasts(address)", acct).abi_return
    low20 = int.from_bytes(decode_address(acct)[-20:], "big")
    # address -> uint160 keeps the 160-bit (low-20) identity; casting back
    # stays inside that namespace.
    assert as_int(as_i) == low20
    def addr_bytes(v):
        return decode_address(v) if isinstance(v, str) else bytes(v)[-32:]
    assert addr_bytes(back)[-20:] == decode_address(acct)[-20:]
    # contract -> address is the app's own ESCROW account (the address(this)
    # canon real contracts key state on).
    from algosdk.logic import get_application_address
    assert addr_bytes(self_addr) == decode_address(
        get_application_address(app.app_id))
    # contract-typed returns wire-encode as the same escrow account.
    got = harness.call(app, "contractCast()").abi_return
    assert addr_bytes(got) == decode_address(get_application_address(app.app_id))

    b2, b8w = harness.call(
        app, "bytesNResize(bytes4)", bytes.fromhex("deadbeef")).abi_return
    assert bytes(b2) == bytes.fromhex("dead")
    assert bytes(b8w) == bytes.fromhex("deadbeef00000000")
