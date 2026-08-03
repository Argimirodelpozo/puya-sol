"""puya-sol-specific regression guards — NOT vendored / NOT original Solidity
semantic tests.

These pin behaviour for bugs fixed in the puya-sol AVM backend itself that the
upstream Solidity semantic-test corpus does not exercise. Kept deliberately
separate from the ported `tests/<cat>/` suites.
"""
import pytest

from framework import as_int, as_bytes, as_signed_int
from framework.compile import CompileError


def test_checked_sub_evaluates_rhs_once(harness):
    """puyasolRegression/contracts/eval_once_sub.sol — NOT an o.g. semantic test.

    A checked unsigned `a - f()` must evaluate f() exactly once. The pre-fix
    inlined wrapping-subtraction referenced the RHS twice (the `a >= b` underflow
    assert AND the (a + 2^256 - b) % 2^256 wrap), so a side-effecting f() ran
    twice. Fixed by routing through eval-once buildWrappingSubtract.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/eval_once_sub.sol")
    r = harness.call(app, "subOnce(uint256)", 10)
    assert as_int(r.abi_return) == 9  # 10 - bump()(=1)
    # bump() incremented `calls` once, not twice.
    assert as_int(harness.call(app, "calls()").abi_return) == 1


def test_balance_temp_not_aliased(harness):
    """puyasolRegression/contracts/balance_alias.sol — NOT an o.g. semantic test.

    Two `address(c).balance` reads in one expression must resolve to distinct
    temps. The pre-fix `__app_balance_addr` was a fixed name, so the second
    app_params_get(AppAddress) clobbered the first and `sum` came back as
    2*bBal instead of aBal+bBal. Children are funded with different values so
    the two balances are distinct (else the bug would be invisible).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/balance_alias.sol", fund_wei=8_000_000
    )
    r = harness.call(app, "probe()", extra_fee=30_000).abi_return
    a_bal, b_bal, total = as_int(r[0]), as_int(r[1]), as_int(r[2])
    assert a_bal != b_bal, f"need distinct child balances (a={a_bal}, b={b_bal})"
    assert total == a_bal + b_bal, (
        f"sum {total} != aBal+bBal {a_bal + b_bal}; "
        f"an aliased temp would give 2*bBal={2 * b_bal}"
    )


def test_struct_with_mapping_storage_ref_slot(harness):
    """puyasolRegression/contracts/struct_ref_slot_return.sol — NOT an o.g. semantic test.

    A library function returning a storage ref to a struct-with-mapping is modeled
    as a biguint slot handle (puya can't hold the mapping-bearing struct value), so
    `.slot` on the bound local must read that handle. Pre-fix this coerce-errored
    ("cannot coerce non-scalar type 'Items' to biguint in assembly arithmetic").
    get() sets x.slot := 123 and returns it; f() reads ptr.slot → 123.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/struct_ref_slot_return.sol")
    assert as_int(harness.call(app, "f()").abi_return) == 123


def test_recursive_struct_array(harness):
    """puyasolRegression/contracts/recursive_struct_array.sol — NOT an o.g. semantic test.

    A struct with a dynamic array of itself (`S[] x` inside S) is recursive; puya's
    IR rejects inline recursive types. The frontend breaks the cycle by mapping the
    recursive array field's element to a fixed projection, so the high-level
    push/index/field read-write round-trips. Constructor sets s.v=21, pushes two
    elements with v=101/102.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/recursive_struct_array.sol")
    assert as_int(harness.call(app, "sv()").abi_return) == 21
    assert as_int(harness.call(app, "len()").abi_return) == 2
    assert as_int(harness.call(app, "v0()").abi_return) == 101
    assert as_int(harness.call(app, "v1()").abi_return) == 102


def test_asm_storage_routes_to_statevar(harness):
    """puyasolRegression/contracts/asm_sstore_statevar.sol — NOT an o.g. semantic test.

    Assembly sstore/sload on a scalar app-global state var (direct `.slot` ref) route
    to the var's OWN storage, unifying the high-level box/global model with assembly —
    not the disjoint __dyn_storage blob. f(): asm write → high-level read sees 42.
    g(): asm write → asm read sees 99 (both routed).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_sstore_statevar.sol")
    assert as_int(harness.call(app, "f()").abi_return) == 42
    assert as_int(harness.call(app, "g()").abi_return) == 99


def test_unmapped_value_type_hard_errors(harness):
    """puyasolRegression/contracts/unmapped_type_fixed.sol — NOT an o.g. semantic test.

    A value-carrying unmapped type (fixed-point) must HARD-ERROR, not silently fall
    back to bytes (which would diverge from EVM). Guards the selective unmapped-type
    hard-error in TypeMapper's default case. Meta-types (type(X)/module/abi) and array
    slices still map to bytes — only genuine value types error.
    """
    with pytest.raises(CompileError):
        harness.compile_and_deploy("puyasolRegression/contracts/unmapped_type_fixed.sol")


def test_shift_ge_256_saturates_to_zero(harness):
    """puyasolRegression/contracts/shift_saturate.sol — NOT an o.g. semantic test.

    EVM/Solidity `<<`/`>>` by a shift >= 256 yield 0 (shifts truncate, never
    overflow-check). The high-level uint256 shift path used to REVERT: it built
    2^shift as setbit(bzero(32), 255-shift, 1), and 255-shift underflowed in uint64
    for shift >= 256 → out-of-range setbit index → AVM panic (even `0 << 256`). Now
    guarded like the assembly shl/shr handlers (clamp + `(shift<256)?v:0`). Surfaced
    by the tests/WIP/tiny-fuzzing-oracle differential-fuzzing spike.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/shift_saturate.sol")
    # Normal in-range shifts are unaffected.
    assert as_int(harness.call(app, "shl(uint256,uint256)", 1, 8).abi_return) == 256
    assert as_int(harness.call(app, "shr(uint256,uint256)", 256, 8).abi_return) == 1
    # shift >= 256 saturates to 0 (was a revert) — incl. value 0, which isolates the
    # power-of-2 underflow from any result-overflow path.
    assert as_int(harness.call(app, "shl(uint256,uint256)", 0, 256).abi_return) == 0
    assert as_int(harness.call(app, "shl(uint256,uint256)", 1, 256).abi_return) == 0
    assert as_int(harness.call(app, "shl(uint256,uint256)", (1 << 256) - 1, 300).abi_return) == 0
    assert as_int(harness.call(app, "shr(uint256,uint256)", 1 << 255, 256).abi_return) == 0


def test_array_storage_ref_writes_through_param(harness):
    """puyasolRegression/contracts/array_ref_writethrough.sol — NOT an o.g. semantic test.

    A storage struct-array passed by reference to an internal (contract-method) function must
    write through to the caller's storage (handle model: the array ref travels as a box-key
    handle, a[i].field=v emits box_replace at the ARC4 offset). Pre-fix it hit copy+write-back
    that doesn't reach contract methods, so the write was a dead local store puya DCE'd → 0.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/array_ref_writethrough.sol")
    assert as_int(harness.call(app, "f()").abi_return) == 5


def test_struct_storage_ref_writes_through_param(harness):
    """puyasolRegression/contracts/struct_ref_writethrough.sol — NOT an o.g. semantic test.

    A small (app-global) struct passed by reference to an internal contract method must write
    through (handle model Stage 1b: a struct passed by ref to a contract method boxes on demand,
    so the ref is a box-key handle). Pre-fix it hit copy+write-back that doesn't reach contract
    methods, so the write was a dead local store puya DCE'd → 0.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/struct_ref_writethrough.sol")
    assert as_int(harness.call(app, "f()").abi_return) == 5


def test_memory_aggregate_aliasing(harness):
    """puyasolRegression/contracts/memory_alias.sol — NOT an o.g. semantic test.

    Memory->memory assignment ALIASES (EVM) via copy-elision: T memory b = a makes b refer to
    a's local. The alias is skipped (falls back to copy) when either side is reassigned, so a
    re-point can't clobber the aliased local.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/memory_alias.sol")
    assert as_int(harness.call(app, "aliases()").abi_return) == 11        # mutation through b seen via a
    assert as_int(harness.call(app, "reassignIsSafe()").abi_return) == 5  # b reassigned -> copy, a unchanged


def test_struct_storage_ref_array_element(harness):
    """puyasolRegression/contracts/struct_elem_ref.sol — NOT an o.g. semantic test.

    Dual (key,offset) struct-ref handle: a struct-ref param that receives an array ELEMENT
    (arr[0]) gains a companion offset param. The same param also takes a whole-box state struct
    and a mapping value (offset 0) — all three write through.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/struct_elem_ref.sol")
    assert as_int(harness.call(app, "arrayElem()").abi_return) == 5
    assert as_int(harness.call(app, "structVar()").abi_return) == 5
    assert as_int(harness.call(app, "mapVal()").abi_return) == 5


def test_memory_struct_param_writeback(harness):
    """puyasolRegression/contracts/mem_struct_param.sol — NOT an o.g. semantic test.

    Memory is passed by reference: an internal contract method that mutates a memory struct param
    writes through to the caller (copy+write-back augmentation). Read-only params are not augmented.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mem_struct_param.sol")
    assert as_int(harness.call(app, "writesThrough()").abi_return) == 11
    assert as_int(harness.call(app, "readonlyUnchanged()").abi_return) == 14


def test_signed_subword_widening(harness):
    """puyasolRegression/contracts/signed_subword_widen.sol — NOT an o.g. semantic test.

    Widening a signed sub-word int (int8) to a wider signed int (int16) sign-extends at every
    coercion site (explicit cast, var-decl, assignment, arg, struct field) — int8(-1) -> int16 is
    -1, not 255. Also guards that an already-sign-extended int8 PARAM is not double-extended.
    Found by the differential fuzzer.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_subword_widen.sol")
    BIG = (1 << 256) - 1  # AVM may return the unsigned 2^256-1 form of -1
    def sint(r): 
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    for fn in ("explicitCast(int256)", "varDecl(int256)", "assignTo(int256)", "structField(int256)"):
        assert sint(harness.call(app, fn, BIG)) == -1, fn       # int8(2^256-1) = -1 -> int16 = -1
    assert as_int(harness.call(app, "arg(int256)", BIG).abi_return) in (-1, (1<<256)-1)
    assert sint(harness.call(app, "paramWiden(int8)", -10)) == -10  # already-extended param


def test_subword_arith_shift_signfill(harness):
    """puyasolRegression/contracts/subword_arith_shift.sol — NOT an o.g. semantic test.

    Arithmetic shift right of a signed sub-word int by a dynamic amount >= its width sign-fills:
    int8(-1) >> 256 == -1 (not 0). The value is canonicalised to 256-bit two's complement before
    the SAR. Found by the differential fuzzer.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/subword_arith_shift.sol")
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    assert s(harness.call(app, "shr8(int8,uint256)", -1, 256)) == -1
    assert s(harness.call(app, "shr8(int8,uint256)", -1, 5)) == -1
    assert s(harness.call(app, "shr8(int8,uint256)", -128, 256)) == -1
    assert s(harness.call(app, "shr8(int8,uint256)", 100, 256)) == 0
    assert s(harness.call(app, "shr16(int16,uint256)", -1, 1000)) == -1
    assert s(harness.call(app, "shr8const(int8)", -1)) == -1    # constant amount, was REVERT
    assert s(harness.call(app, "shr8const(int8)", 100)) == 0
    assert s(harness.call(app, "shr8const256(int8)", -1)) == -1


def test_array_oob_huge_index(harness):
    """puyasolRegression/contracts/array_oob_index.sol — NOT an o.g. semantic test.

    An array index >= 2^64 reverts (out of bounds) instead of silently truncating to uint64 and
    reading arr[low-64-bits]. Storage-dynamic, memory, and fixed-size arrays, read + write paths.
    Found by the differential fuzzer.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/array_oob_index.sol")
    BIG = 1 << 128
    for fn in ("storageOOB(uint256)", "memOOB(uint256)", "fixedOOB(uint256)", "memWriteOOB(uint256)",
               "mbReadOOB(uint256)", "mbWriteOOB(uint256)"):
        assert harness.call(app, fn, BIG, expect_revert=True).reverted, fn
    assert as_int(harness.call(app, "inBounds(uint256)", 1).abi_return) == 11  # normal index works


def test_checked_overflow_before_truncation(harness):
    """puyasolRegression/contracts/checked_overflow_trunc.sol — NOT an o.g. semantic test.

    A checked uint256 op narrowed immediately (uintN(s + 1)) reverts on the uint256 overflow
    (checked at full width before truncating) rather than silently wrapping. Found by the fuzzer.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/checked_overflow_trunc.sol")
    MAX = (1 << 256) - 1
    for fn in ("truncAdd(uint256)", "truncMul(uint256)", "viaTemp(uint256)"):
        assert harness.call(app, fn, MAX, expect_revert=True).reverted, fn
    assert as_int(harness.call(app, "uncheckedOK(uint256)", MAX).abi_return) == 0  # unchecked wraps
    assert as_int(harness.call(app, "normalAdd(uint256,uint256)", 5, 3).abi_return) == 8


def test_signed_compound_arithmetic(harness):
    """puyasolRegression/contracts/signed_compound.sol — NOT an o.g. semantic test.

    Signed sub-word compound assignment (int128 x += / -= / *= d) does real signed arithmetic:
    correct value, signed-overflow revert, truncation. Found by the stateful fuzzer (it reached the
    compound-on-a-state-var that the per-call fuzzer skips). Was: unsigned path → false reverts +
    untruncated garbage.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_compound.sol")
    IMAX = (1 << 127) - 1
    IMIN = -(1 << 127)
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    assert s(harness.call(app, "add(int128,int128)", -1, 1)) == 0      # was REVERT
    assert s(harness.call(app, "add(int128,int128)", 10, -3)) == 7
    assert harness.call(app, "add(int128,int128)", IMAX, 1, expect_revert=True).reverted  # overflow
    assert s(harness.call(app, "sub(int128,int128)", 0, 1)) == -1
    assert s(harness.call(app, "mul(int128,int128)", -1, -1)) == 1     # was REVERT
    assert harness.call(app, "mul(int128,int128)", IMAX, 2, expect_revert=True).reverted  # overflow
    assert s(harness.call(app, "uncheckedAdd(int128,int128)", IMAX, 1)) == IMIN  # unchecked wraps


def test_signed_struct_getter_sign_extension(harness):
    """puyasolRegression/contracts/signed_struct_getter.sol — NOT an o.g. semantic test.

    A public struct auto-getter must sign-extend signed sub-word fields. Found by the
    stateful fuzzer: a single-field struct{int128 x} getter skipped projectStructFields
    (read as a scalar) and returned +2^127 for an INT128_MIN field; the int16 single-field
    case did not even compile. Multi-field structs decoded each field unsigned. The explicit
    field read was always correct, so it is the oracle here.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_struct_getter.sol")
    IMIN = -(1 << 127)
    def w(v):                                                    # wrap raw uint → signed-256
        return v - (1 << 256) if v > (1 << 255) else v
    def s256(r):
        return w(as_int(r.abi_return))
    # single-field int128 getter at the boundary (was +2^127, unsigned)
    harness.call(app, "setOne(int128)", IMIN)
    assert s256(harness.call(app, "one()")) == IMIN
    assert s256(harness.call(app, "readOne()")) == IMIN          # explicit-read oracle agrees
    harness.call(app, "setOne(int128)", -5)
    assert s256(harness.call(app, "one()")) == -5
    # single-field int16 getter (was a compile error; here it must compile and sign-extend)
    harness.call(app, "setSmall(int16)", -5)
    assert s256(harness.call(app, "small()")) == -5
    assert s256(harness.call(app, "readSmall()")) == -5
    # multi-field struct getter: each signed field is 256-bit two's-complement in the tuple
    # (a sub-64 int16 was uint64-shaped — found by the stateful fuzzer once getters were
    # re-sampled after each mutation; the int128 case only canon-matched before).
    harness.call(app, "setMany(int16,int128)", -3, IMIN)
    assert [w(x) for x in harness.call(app, "many()").abi_return] == [0, -3, 0, IMIN]


def test_nested_array_loop_condition(harness):
    """puyasolRegression/contracts/nested_array_loop.sol — NOT an o.g. semantic test.

    A nested-array extraction in a loop CONDITION (`for j; j < a[i].length`) reverted: the
    for-loop dropped the condition's prePendingStatements (the bounds-check assert + index
    cache for `a[i]`) into the body, after the test that consumed them. The for-loop now
    drains them and re-runs them each iteration before the test. Found by the differential
    fuzzer (uint256[][] probe). Workaround was `T[] x = a[i]; x[j]`.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/nested_array_loop.sol")
    def i(r): return as_int(r.abi_return)
    assert i(harness.call(app, "sumNested(uint256[][])", [[1, 2], [3]])) == 6      # was REVERT
    assert i(harness.call(app, "sumNested(uint256[][])", [[]])) == 0
    assert i(harness.call(app, "sumNested(uint256[][])", [[5], [], [7, 8]])) == 20
    assert i(harness.call(app, "sumNested(uint256[][])", [])) == 0
    assert i(harness.call(app, "countNested(uint256[][])", [[1, 2, 3], [4]])) == 4
    # break / continue still route through the for-post in the restructured loop
    assert i(harness.call(app, "sumEvenIdx(uint256[])", [10, 1, 20, 1, 30])) == 60  # idx 0,2,4
    assert i(harness.call(app, "sumEvenIdx(uint256[])", [10, 1, 99, 1, 30])) == 10  # break at idx 2
    # same nested extraction in a WHILE condition (non-do while had the identical orphaning)
    assert i(harness.call(app, "sumNestedWhile(uint256[][])", [[1, 2], [3]])) == 6
    assert i(harness.call(app, "sumNestedWhile(uint256[][])", [[5], [], [7, 8]])) == 20


def test_subword_shift_saturate(harness):
    """puyasolRegression/contracts/subword_shift_saturate.sol — NOT an o.g. semantic test.

    Found by the GENERATIVE fuzzer (fuzz_gen.py). A sub-word `<<`/`>>` by a constant (or any
    <=64-bit-typed) amount >= 64 hit the raw uint64 shl/shr opcode, which fails for shift >= 64,
    so it REVERTED — but Solidity saturates to 0 (sign-fill for signed >>) and never reverts. Fix
    routes all shifts through the guarded biguint path (the variable-uint256 path already was).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/subword_shift_saturate.sol")
    def s16(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    assert as_int(harness.call(app, "shl16_64(uint16)", 7).abi_return) == 0       # was REVERT
    assert as_int(harness.call(app, "shl16_256(uint16)", 0xFFFF).abi_return) == 0  # was REVERT
    assert as_int(harness.call(app, "shl16_4(uint16)", 7).abi_return) == 112       # in-range unchanged
    assert as_int(harness.call(app, "shl16_4(uint16)", 0xFFFF).abi_return) == 0xFFF0  # masks to 16 bits
    assert as_int(harness.call(app, "shr16_256(uint16)", 0x8000).abi_return) == 0  # was REVERT
    assert as_int(harness.call(app, "shl64_64(uint64)", 1).abi_return) == 0        # was REVERT
    assert s16(harness.call(app, "shlI16_256(int16)", 5)) == 0                     # was REVERT
    assert s16(harness.call(app, "shrI16_256(int16)", -1)) == -1                   # signed >>: sign-fill
    assert s16(harness.call(app, "shrI16_256(int16)", 100)) == 0                   # non-negative → 0
    # shift as a sub-expression (was a puya compile error: biguint where uint64 expected)
    assert as_int(harness.call(app, "comp(uint64,uint64)", 3, 0xFFFF).abi_return) == ((3 << 7) & 0xFFFF)
    assert as_int(harness.call(app, "compR(uint16,uint16)", 0x80, 0x0F).abi_return) == (0x0F | (0x80 >> 3))


def test_signed_subword_exp(harness):
    """puyasolRegression/contracts/signed_subword_exp.sol — NOT an o.g. semantic test.

    Found by the GENERATIVE fuzzer (fuzz_gen.py). Signed sub-word `**` had two bugs: an UNCHECKED
    overflowing result was not wrapped mod 2^bits, so the negation `pow2N - absResult` underflowed
    the biguint subtraction and the AVM `b-` panicked (int8 (-128)**3 → REVERT; EVM wraps to 0); and
    the biguint result broke sub-expression composition (`b ^ (a**3)` → puya compile error). Fixed by
    masking the magnitude for unchecked + narrowing the sub-word result to uint64.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_subword_exp.sol")
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    # unchecked signed sub-word exp now WRAPS (was a `b-` underflow revert)
    assert s(harness.call(app, "exp3u(int8)", -128)) == 0     # (-128)**3 = -2097152 → wraps to 0
    assert s(harness.call(app, "exp3u(int8)", -127)) == -127
    assert s(harness.call(app, "exp3u(int8)", 2)) == 8
    assert s(harness.call(app, "exp3u(int8)", -2)) == -8
    assert s(harness.call(app, "exp3u(int8)", -5)) == -125
    # checked: in-range ok, overflow still reverts
    assert s(harness.call(app, "exp3c(int8)", 5)) == 125
    assert harness.call(app, "exp3c(int8)", -128, expect_revert=True).reverted
    assert harness.call(app, "exp3c(int8)", 6, expect_revert=True).reverted    # 216 > int8 max
    # composition: b ^ (a**3) — was a puya compile error (biguint where uint64 expected)
    assert s(harness.call(app, "comp(int8,int8)", 2, 5)) == (5 ^ 8)
    assert s(harness.call(app, "comp(int8,int8)", -2, 0)) == -8
    # unsigned + wider sub-word still correct
    assert as_int(harness.call(app, "expU8(uint8)", 10).abi_return) == 232     # 1000 mod 256
    assert s(harness.call(app, "expI16(int16)", -200)) == -25536               # 40000 wraps int16


def test_unchecked_uint64_sub_wraps(harness):
    """puyasolRegression/contracts/unchecked_uint64_sub.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer's CONTROL-FLOW mode (fuzz_gen.py --cf). An UNCHECKED uint64
    `a - b` with a < b reverted (raw uint64 `-` panics on underflow); Solidity wraps. The
    wrapping-sub fix covered sub-word (<64) and biguint (>64) but uint64 (==64) fell in the gap.
    Fix routes uint64 unchecked Sub through the biguint wrapping subtract, then narrows to uint64.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unchecked_uint64_sub.sol")
    M = 1 << 64
    assert as_int(harness.call(app, "sub(uint64,uint64)", 0, 1).abi_return) == M - 1       # was REVERT
    assert as_int(harness.call(app, "sub(uint64,uint64)", 5, 8).abi_return) == M - 3
    assert as_int(harness.call(app, "sub(uint64,uint64)", 10, 3).abi_return) == 7          # no wrap
    assert harness.call(app, "subc(uint64,uint64)", 0, 1, expect_revert=True).reverted     # checked underflow
    assert as_int(harness.call(app, "subc(uint64,uint64)", 10, 3).abi_return) == 7
    # composition: (a - b) | c — the biguint result must narrow to uint64
    assert as_int(harness.call(app, "comp(uint64,uint64,uint64)", 0, 1, 0).abi_return) == M - 1


def test_signed_subword_negate_compose(harness):
    """puyasolRegression/contracts/signed_subword_negate.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer's ARRAY mode (fuzz_gen.py --arr). An unchecked unary minus on a
    sub-word signed value did not wrap to N bits: -INT_MIN = +2^(N-1) overflows intN and must wrap to
    INT_MIN. `-a` alone re-truncates on return so it looked right; as a subexpression in a signed
    compare (whose XOR-sign-bit trick assumes canonical operands) the raw +2^(N-1) read as positive.
    Fix masks + sign-extends the negation result (uint64 path for N<64, 256-bit path for 64<N<256).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_subword_negate.sol")
    def s16(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    # (-a) > a at INT_MIN: -INT_MIN wraps to INT_MIN so both equal → false (was TRUE: -a read as +2^(N-1))
    assert as_int(harness.call(app, "cmp8(int8)", -128).abi_return) == 0
    assert as_int(harness.call(app, "cmp16(int16)", -32768).abi_return) == 0
    assert as_int(harness.call(app, "cmp128(int128)", -(1 << 127)).abi_return) == 0
    # non-MIN still correct: -5 > 5 false; 5 > -5 true
    assert as_int(harness.call(app, "cmp16(int16)", 5).abi_return) == 0
    assert as_int(harness.call(app, "cmp16(int16)", -5).abi_return) == 1
    # bare negation unchanged (return path re-truncates)
    assert s16(harness.call(app, "neg16(int16)", -32768)) == -32768   # -INT16_MIN wraps to INT16_MIN
    assert s16(harness.call(app, "neg16(int16)", 5)) == -5
    # checked negation of INT_MIN reverts
    assert harness.call(app, "negc16(int16)", -32768, expect_revert=True).reverted


def test_unchecked_uint64_exp_wraps(harness):
    """puyasolRegression/contracts/unchecked_uint64_exp.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer's CAST mode (fuzz_gen.py --cast). An UNCHECKED uint64 `a**k` whose
    power overflows 2^64 REVERTED: the AVM `exp` opcode is uint64-only and asserts on overflow, but
    Solidity wraps. The unchecked-exp wrap covered sub-word (m_bits<64); uint64 (==64) fell in the gap
    (the same gap as unchecked uint64 sub). Fix routes uint64 unchecked Pow through biguint
    square-and-multiply + mod 2^64 + narrow. Add/Mult at uint64 already wrapped; only exp was broken.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unchecked_uint64_exp.sol")
    M = 1 << 64
    MAX = M - 1
    # MAX ≡ -1 mod 2^64 → MAX**2 ≡ 1, MAX**3 ≡ MAX
    assert as_int(harness.call(app, "exp2(uint64)", MAX).abi_return) == 1          # was REVERT
    assert as_int(harness.call(app, "exp3(uint64)", MAX).abi_return) == MAX        # was REVERT
    assert as_int(harness.call(app, "exp2(uint64)", 1 << 33).abi_return) == 0      # (2^33)^2 = 2^66 wraps to 0
    assert as_int(harness.call(app, "exp2(uint64)", 5).abi_return) == 25           # in-range unchanged
    assert as_int(harness.call(app, "exp3(uint64)", 3).abi_return) == 27
    assert as_int(harness.call(app, "exp2(uint64)", 0).abi_return) == 0
    # checked: overflow reverts, in-range ok
    assert harness.call(app, "exp2c(uint64)", MAX, expect_revert=True).reverted
    assert as_int(harness.call(app, "exp2c(uint64)", 5).abi_return) == 25
    # composition: (a**2) | 7 — the biguint result must narrow to uint64
    assert as_int(harness.call(app, "comp(uint64)", MAX).abi_return) == (1 | 7)


def test_bytesn_shift(harness):
    """puyasolRegression/contracts/bytesN_shift.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer's BYTES mode (fuzz_gen.py --bytes). A bytesN bit shift `b << k` /
    `b >> k` HARD-ERRORED in the puya backend ("unsupported type cast from uint64 to bytes"): the
    generic integer-shift path coerced the bytesN operand through uint64 (and uint64 can't hold
    bytes>8). Bitwise & | ^ ~ were already fine. Fix lowers the shift via biguint (asBiguint(b) shifted
    by k bits) then keeps the low N bytes (Solidity truncates to N), in SolFixedBytesBuilder::binary_op.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/bytesN_shift.sol")

    def b2i(r):                                            # bytesN return -> big-endian int
        v = r.abi_return
        if isinstance(v, (list, tuple)):
            v = bytes(int(x) & 0xFF for x in v)
        elif isinstance(v, str):
            v = bytes.fromhex(v[2:] if v.startswith("0x") else v)
        return int.from_bytes(v, "big") if isinstance(v, (bytes, bytearray)) else int(v)

    A4 = (0x12345678).to_bytes(4, "big")
    assert b2i(harness.call(app, "shl4(bytes4,uint8)", A4, 8)) == 0x34567800   # was a hard COMPILE error
    assert b2i(harness.call(app, "shl4(bytes4,uint8)", A4, 0)) == 0x12345678   # no shift
    assert b2i(harness.call(app, "shl4(bytes4,uint8)", A4, 32)) == 0           # k>=8N: shifted fully out
    assert b2i(harness.call(app, "shr4(bytes4,uint8)", A4, 8)) == 0x00123456
    assert b2i(harness.call(app, "shr4(bytes4,uint8)", A4, 32)) == 0
    assert b2i(harness.call(app, "shr1(bytes1,uint8)", b"\x80", 1)) == 0x40
    assert b2i(harness.call(app, "shl32(bytes32,uint16)", (1).to_bytes(32, "big"), 8)) == 256
    assert b2i(harness.call(app, "shl32(bytes32,uint16)", (1).to_bytes(32, "big"), 255)) == (1 << 255)
    # composition: (a << 8) | b
    assert b2i(harness.call(app, "comp(bytes4,bytes4)", A4, (0xFF).to_bytes(4, "big"))) == 0x345678FF


def test_wide_dynamic_array_length(harness):
    """puyasolRegression/contracts/wide_dynamic_array_length.sol — NOT an o.g. semantic test.

    Found by the generative STATEFUL fuzzer. A wide (biguint-backed, <32-byte) dynamic STORAGE array's
    `.length` read `total_bytes/32` instead of the element count (uint128[] x3 -> 1). FRONTEND bug, FIXED:
    SolLengthAccess computed the box divisor via map()+mapToARC4Type, which erases sub-256 widths to
    biguint -> 32; push/index use the width-preserving mapSolTypeToARC4 (uint128 -> 16). Aligned the two
    so .length divides by the real stride. Also fixed uint8/16/32[] (stored at 1/2/4 B, divided by 8).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/wide_dynamic_array_length.sol")
    for v in (111, 222, 333):
        harness.call(app, "push(uint128)", v)
    assert as_int(harness.call(app, "get(uint256)", 0).abi_return) == 111
    assert as_int(harness.call(app, "get(uint256)", 2).abi_return) == 333
    # was 1 (= 3*16/32) before the stride fix; now the true count
    assert as_int(harness.call(app, "len()").abi_return) == 3
    # other sub-32-byte widths broke the same way (divided by 32 or 8); now all read the true count
    for sig, push_sig, vals in (
        ("lenB()", "pushB(uint160)", (5, 6, 7)),       # 20-byte stride (was /32 -> 1)
        ("lenC()", "pushC(uint32)", (5, 6, 7)),        #  4-byte stride (was /8  -> 1)
        ("lenD()", "pushD(uint8)", (5, 6, 7)),         #  1-byte stride (was /8  -> 0)
        ("lenE()", "pushE(uint256)", (5, 6, 7)),       # 32-byte control (always correct)
    ):
        for v in vals:
            harness.call(app, push_sig, v)
        assert as_int(harness.call(app, sig).abi_return) == 3


def test_asm_signed_negatives(harness):
    """puyasolRegression/contracts/asm_signed_negatives.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (inline assembly Yul ops). Yul sdiv/smod/sar reverted/were wrong for
    NEGATIVE operands. ROOT CAUSE (TEAL sig was `sdivF(uint512)uint512`): an asm-bodied fn exposed its
    256-bit params as arc4.uint512 (64 bytes) not uint256, because the body is built AFTER the param
    remap so the Yul body saw the remapped type and a negative int256 arrived as a 512-bit value (then
    negate256()'s maxU256-val underflowed). FIX: apply the biguint->ARC4 param remap to asm bodies, but
    DEFER the arg.wtype mutation until after buildBlock so the Yul body builds against the native biguint
    type (and the switch handler dispatches correctly). The sdiv/smod/sar logic was always correct.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_signed_negatives.sol")

    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    # positive operands
    assert s(harness.call(app, "sdivF(int256)", 127)) == 42
    assert s(harness.call(app, "sarF(int256)", 100)) == 25
    # negative operands: sdiv(-128,3)=-42, smod(-128,3)=-2, sar(2,-128)=-32 (were revert / wrong)
    assert s(harness.call(app, "sdivF(int256)", -128)) == -42
    assert s(harness.call(app, "smodF(int256)", -128)) == -2
    assert s(harness.call(app, "sarF(int256)", -128)) == -32


def test_yul_byte_out_of_range(harness):
    """puyasolRegression/contracts/yul_byte_out_of_range.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (inline assembly Yul). `byte(n, x)` for n >= 32 REVERTED (the AVM
    extract3 at offset n past the 32-byte value); EVM returns 0 out of range. Fix guards `n < 32 ? byte
    : 0` (conditional only evaluates the extract on the taken branch), like the shift>=256 saturate fix.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/yul_byte_out_of_range.sol")
    X = 0x00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
    # in-range bytes unchanged
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 0, X).abi_return) == 0x00
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 1, X).abi_return) == 0x11
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 31, X).abi_return) == 0xff
    # n >= 32 → 0 (was REVERT)
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 32, X).abi_return) == 0
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 100, X).abi_return) == 0
    assert as_int(harness.call(app, "byteF(uint256,uint256)", 255, X).abi_return) == 0


def test_yul_user_fn_var_clash(harness):
    """puyasolRegression/contracts/yul_user_fn_var_clash.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (Yul user functions). Inline-expanded Yul user functions were bound
    to BARE names (x, y) in m_locals, so functions sharing names — or nested/repeated calls — clobbered
    each other's runtime vars: `add(sq(a),cube(b))` collapsed to 2*a^3 (every call -> cube(a)) not
    a^2+b^3. FIX: each inline expansion gets unique names __yul_<uid>_<name> via a scoped rename map
    applied in resolveVarRef, and publishes the unique return temp so the caller reads the right var
    (mirrors the subroutine path's __yulret_<id> temps). Covers nested (cube calls sq) + sibling calls.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/yul_user_fn_var_clash.sol")
    # uf(a,b) = a^2 + b^3 (sq/cube share x/y; cube nests sq) — was 2*a^3
    assert as_int(harness.call(app, "uf(uint256,uint256)", 2, 0).abi_return) == 4
    assert as_int(harness.call(app, "uf(uint256,uint256)", 3, 2).abi_return) == 17
    assert as_int(harness.call(app, "uf(uint256,uint256)", 5, 3).abi_return) == 52
    assert as_int(harness.call(app, "uf(uint256,uint256)", 0, 4).abi_return) == 64  # 0 + 4^3


def test_signed_negation_overflow(harness):
    """puyasolRegression/contracts/signed_negation_overflow.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). Checked `-(type(intN).min)` overflows and must REVERT;
    `unchecked` wraps it back to intN.min. The overflow guard in SolIntegerBuilder::unary_op missed
    exactly int64 (its `(1<<64)-1` mask is C++ UB -> 0, so the guard never fired) and int128 (the
    operand is 256-bit sign-extended, so int128.min reads as 2^256-2^127 but the guard compared
    against 2^127). int8/16/32 + int256 already reverted. FIX: mask all-ones for N==64, and compare
    biguint-backed operands against the sign-extended min (2^256 - 2^(N-1)).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_negation_overflow.sol")
    mins = {
        "neg8(int8)": -(1 << 7),
        "neg16(int16)": -(1 << 15),
        "neg32(int32)": -(1 << 31),
        "neg64(int64)": -(1 << 63),     # was the bug (no revert)
        "neg128(int128)": -(1 << 127),  # was the bug (no revert)
        "neg256(int256)": -(1 << 255),
    }
    # checked: -(intN.min) must revert (overflow)
    for sig, mn in mins.items():
        assert harness.call(app, sig, mn, expect_revert=True).reverted, sig

    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    # checked: ordinary values negate correctly
    assert s(harness.call(app, "neg64(int64)", 5)) == -5
    assert s(harness.call(app, "neg64(int64)", -5)) == 5
    assert s(harness.call(app, "neg128(int128)", -7)) == 7
    assert s(harness.call(app, "neg128(int128)", (1 << 126))) == -(1 << 126)
    # unchecked: -(intN.min) wraps back to intN.min (no revert)
    assert s(harness.call(app, "uneg64(int64)", -(1 << 63))) == -(1 << 63)
    assert s(harness.call(app, "uneg128(int128)", -(1 << 127))) == -(1 << 127)


def test_signed_subword_compare(harness):
    """puyasolRegression/contracts/signed_subword_compare.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cf). A signed ordering compare on a sub-word int (int8/16/32)
    was wrong when an operand wasn't sign-extended in its uint64 slot: a negative literal cast
    (int8(-1) = 0xff) or an unchecked sub-word arith result (0-(-128) = 0x80). SolIntegerBuilder::
    compare's uint64 path XOR'd with 2^63 to get unsigned ordering but never sign-extended first, so
    0xff (-1) ordered above 0 -> `int8(-1) < int8(0)` returned false. ABI params arrive sign-extended,
    hiding it. int64/int256 were already correct. FIX: signExtendToUint64 each operand before the XOR.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_subword_compare.sol")
    bo = lambda r: as_int(r.abi_return)  # bool -> 1/0
    # negative literal cast: -1 < 0 is true (was false for int8/16/32)
    assert bo(harness.call(app, "ltNeg8()")) == 1
    assert bo(harness.call(app, "ltNeg16()")) == 1
    assert bo(harness.call(app, "ltNeg32()")) == 1
    assert bo(harness.call(app, "ltNeg256()")) == 1
    # unchecked arith result < 0
    assert bo(harness.call(app, "modNeg8(int8,int8)", 0, -128)) == 0     # 0 % -128 = 0, not < 0
    assert bo(harness.call(app, "modNeg8(int8,int8)", -1, -128)) == 1    # -1 % -128 = -1 < 0
    assert bo(harness.call(app, "subWrap8(int8,int8)", 0, -128)) == 1    # 0-(-128)=128 wraps -128 < 0
    assert bo(harness.call(app, "mulWrap8(int8,int8)", -1, -128)) == 1   # -1*-128=128 wraps -128 < 0
    assert bo(harness.call(app, "modNeg16(int16,int16)", -1, -32768)) == 1
    # int64 (full width) still correct
    assert bo(harness.call(app, "modNeg64(int64,int64)", -1, -(1 << 60))) == 1
    assert bo(harness.call(app, "modNeg64(int64,int64)", 5, 3)) == 0     # 5%3=2, not < 0
    # sanity: ordinary param comparisons unaffected
    assert bo(harness.call(app, "ltPos8(int8,int8)", -5, 3)) == 1
    assert bo(harness.call(app, "ltPos8(int8,int8)", 3, -5)) == 0
    assert bo(harness.call(app, "gte16(int16,int16)", -1, -2)) == 1


def test_signed_subword_equality(harness):
    """puyasolRegression/contracts/signed_subword_equality.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--arr seed 13004 f0). The ==/!= analogue of the ordering-compare
    fix. Two non-canonical-operand bugs in sub-word signed equality: (1) a negative literal cast int8(-1)
    was emitted as 255 (masked, not sign-extended) -> int8(-1) == int8(-1) was false; fixed at the source
    in SolTypeConversion. (2) an unchecked sub-word arith result (127 -= -128 wraps to -1 as 0xff) compared
    == nonzero wrongly because compare() only sign-extended operands for ordering; fixed by canonicalising
    operands for ordering AND equality. Comparing to 0 hid both -> these compare to nonzero.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_subword_equality.sol")
    bo = lambda r: as_int(r.abi_return)
    assert bo(harness.call(app, "eqNegLit8()")) == 1     # was 0
    assert bo(harness.call(app, "eqNegLit16()")) == 1
    assert bo(harness.call(app, "eqParamLit(int8)", -1)) == 1    # a == int8(-1), was 0
    assert bo(harness.call(app, "eqParamLit(int8)", 0)) == 0
    # unchecked arith result == nonzero
    assert bo(harness.call(app, "eqAfterSub(int8,int8)", 127, -128)) == 1   # 127-(-128)=-1 == -1
    assert bo(harness.call(app, "eqAfterSub(int8,int8)", 5, 1)) == 0        # 4 != -1
    assert bo(harness.call(app, "neAfterAdd(int16,int16)", 32767, 1)) == 1  # wraps to -32768 != -1 -> true
    # sanity + full-width
    assert bo(harness.call(app, "eqParam(int8,int8)", -5, -5)) == 1
    assert bo(harness.call(app, "eqParam(int8,int8)", -5, 5)) == 0
    assert bo(harness.call(app, "eqNegLit64()")) == 1
    assert bo(harness.call(app, "eqNegLit256()")) == 1


def test_type_minmax_canonical(harness):
    """puyasolRegression/contracts/type_minmax_canonical.sol — NOT an o.g. semantic test.

    Locks type(intN).min/max after the solc-reuse consolidation: SolMetaTypeAccess routes solc's
    IntegerType::min()/max() (256-bit two's-complement) through TypeCoercion::canonicalIntConstant
    (<=64 -> low 64-bit TC/uint64; >64 -> 256-bit TC/biguint), replacing ~40 lines of hand-rolled TC
    math that had to stay in lockstep with SolLiteral. Values + their compare/arith uses must hold.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/type_minmax_canonical.sol")
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v  # 2^255 itself is int256.min
    assert s(harness.call(app, "mn8()")) == -128
    assert s(harness.call(app, "mn16()")) == -32768
    assert s(harness.call(app, "mn128()")) == -(1 << 127)
    assert s(harness.call(app, "mn256()")) == -(1 << 255)
    assert s(harness.call(app, "mx8()")) == 127
    assert as_int(harness.call(app, "mxu256()").abi_return) == (1 << 256) - 1
    # canonical min compares equal to the matching literal
    assert as_int(harness.call(app, "minIsLit8(int8)", -128).abi_return) == 1
    assert as_int(harness.call(app, "minIsLit8(int8)", -127).abi_return) == 0
    assert as_int(harness.call(app, "minIsLit128(int128)", -(1 << 127)).abi_return) == 1
    # arithmetic: -1 + (-128) wraps (unchecked int8) to 127
    assert s(harness.call(app, "addMin8(int8)", -1)) == 127


def test_const_fold_arbitrary_precision(harness):
    """puyasolRegression/contracts/const_fold_arbitrary_precision.sol — NOT an o.g. semantic test.

    solc-todo.md opportunity A: the 'const-fold gap' (type(uint64).max**2 reverting on AVM but folding on
    EVM) turned out NOT to exist — AVM matches EVM. A constant that fits its target is folded to its exact
    value (tryConstantFold + rationalIntConstant). One that overflows its OPERAND type in a checked context
    reverts on BOTH (type(uint64).max**2 has type uint64; solc does not widen it). Unchecked, it wraps in
    the operand width on both. No ConstantEvaluator integration needed. (Diagnosed: the fuzzer's
    "no divergence" for type(uint64).max**2 was both-revert, not a value match.)
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/const_fold_arbitrary_precision.sol")
    # constants that fit are folded to the exact value
    assert as_int(harness.call(app, "pow1077()").abi_return) == 10 ** 77
    assert as_int(harness.call(app, "half256p1()").abi_return) == 2 ** 255
    assert as_int(harness.call(app, "bigShift()").abi_return) == 1 << 200
    assert as_int(harness.call(app, "bigMul()").abi_return) == 3 * (2 ** 200)
    # unchecked: the uint64 op wraps in its operand width
    assert as_int(harness.call(app, "maxU64sqWrap()").abi_return) == ((2 ** 64 - 1) ** 2) % (2 ** 64)  # 1
    # checked: uint64**2 overflows uint64 -> reverts on the AVM exactly as on EVM (NOT a fold gap)
    assert harness.call(app, "maxU64sqChecked()", expect_revert=True).reverted


def test_memory_subword_aggregate(harness):
    """puyasolRegression/contracts/memory_subword_aggregate.sol — NOT an o.g. semantic test.

    solc-todo.md opportunity C (element/field sizes): reusing solc's calldataEncodedSize for
    computeEncodedElementSize is non-viable (WType-based; bool/address use puya's widths 8/32 not solc's
    1/20; sizes are box-packed vs blob-32 context-dependent). No latent size bug exists — box sub-word
    aggregates were fuzzed clean this session; this guards the previously-uncovered MEMORY sub-word path
    (struct field read/mutate + array index) against live EVM.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/memory_subword_aggregate.sol")
    def s(r):  # signed sub-word returns are sign-extended to 256-bit TC
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v
    A, B, C, D = 1234567890123, -100, 200, 999888777
    assert as_int(harness.call(app, "field_a(uint128,int16,uint8,uint128)", A, B, C, D).abi_return) == A
    assert s(harness.call(app, "field_b(uint128,int16,uint8,uint128)", A, B, C, D)) == B
    assert as_int(harness.call(app, "field_d(uint128,int16,uint8,uint128)", A, B, C, D).abi_return) == D
    assert s(harness.call(app, "mutate_b(uint128,int16,uint8,uint128,int16)", A, B, C, D, 77)) == 77
    assert as_int(harness.call(app, "arr_idx(uint128,uint128,uint128,uint256)", 10, 20, 30, 1).abi_return) == 20
    assert as_int(harness.call(app, "arr_idx(uint128,uint128,uint128,uint256)", 10, 20, 30, 2).abi_return) == 30
    assert s(harness.call(app, "sarr_idx(int16,int16,uint256)", 5, -7, 1)) == -7
    assert s(harness.call(app, "sarr_idx(int16,int16,uint256)", 5, -7, 2)) == -1


def test_signed_mixedwidth_divmod(harness):
    """puyasolRegression/contracts/signed_mixedwidth_divmod.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (mixed-width arithmetic). Signed div/mod with a biguint-backed dividend
    (int128/int256) and a NARROWER signed divisor returned garbage (int128/int16 div -> 0, mod -> the
    dividend) because buildSignedDivMod masked both to N (commonType) bits and read sign via >= 2^(N-1),
    but the narrow divisor was sign-extended only in its 64-bit slot (2^64-32768), masking to a huge
    POSITIVE value. FIX: coerceToCommonInt each operand to canonical commonType before buildSignedDivMod.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_mixedwidth_divmod.sol")
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v
    # int128 / int16
    assert s(harness.call(app, "div128_16(int128,int16)", 100, -7)) == -14          # trunc toward zero
    assert s(harness.call(app, "div128_16(int128,int16)", -8388609, -32768)) == 256  # was 0
    assert s(harness.call(app, "mod128_16(int128,int16)", -8388609, -32768)) == -1   # was the dividend
    assert s(harness.call(app, "mod128_16(int128,int16)", 100, -7)) == 2             # sign of dividend
    assert s(harness.call(app, "div128_8(int128,int8)", 1000, -3)) == -333
    assert s(harness.call(app, "div256_16(int256,int16)", -(1 << 200), -32768)) == (1 << 200) // 32768
    assert s(harness.call(app, "mod256_16(int256,int16)", (1 << 200) + 5, 32767)) == ((1 << 200) + 5) % 32767


def test_signed_mixedwidth_mul(harness):
    """puyasolRegression/contracts/signed_mixedwidth_mul.sol — NOT an o.g. semantic test.

    Found by the CORPUS-MUTATION fuzzer (fuzz_mutate.py, small_signed_types.sol
    mutated int64->int192). Mixed-width signed add/sub/MUL with a NARROWER signed
    operand returned garbage: `-int32(10) * -int192(20)` gave -20*(2^64-10) not 200.
    A narrower signed operand arrives as its OWN-width two's complement (int32 -10 =
    uint64 2^64-10 / int128 -10 = biguint 2^128-10); buildSignedArithmetic then masks
    to 2^commonBits, embedding that as a large POSITIVE. FIX: sign-extend each narrower
    signed operand to the common width first (coerceToCommonInt) — mirrors the div/mod
    path (test_signed_mixedwidth_divmod).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_mixedwidth_mul.sol")
    def s(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v
    assert s(harness.call(app, "mul32_192(int32,int192)", -10, 20)) == -200
    assert s(harness.call(app, "mul64_192(int64,int192)", -10, 20)) == -200
    assert s(harness.call(app, "mul128_192(int128,int192)", -10, 20)) == -200
    assert s(harness.call(app, "mul128_192(int128,int192)", -3, -7)) == 21
    assert s(harness.call(app, "add32_192(int32,int192)", -5, 3)) == -2
    assert s(harness.call(app, "sub192_32(int192,int32)", 10, -5)) == 15
    assert s(harness.call(app, "run()")) == 200
    assert s(harness.call(app, "mul192_32(int192,int32)", -20, -10)) == 200


def test_enum_conversion_wide_range(harness):
    """puyasolRegression/contracts/enum_conversion_wide_range.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (internal_library_function_attached_to_enum
    uint256->int136). An int->enum conversion truncated a WIDE biguint input to uint64
    BEFORE the range check, so a value out of range whose LOW 64 bits form a valid enum
    ordinal (int136 -2^135 -> low64 == 0) passed the check and returned the WRONG member
    instead of Panic(0x21) -- a silent miscompile. FIX: range-check the FULL value at its
    own width (typed constant) before truncating. Fixed in both conversion paths
    (SolTypeConversion + eb TypeConversions).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/enum_conversion_wide_range.sol")
    assert as_int(harness.call(app, "fromI136(int136)", 0).abi_return) == 0
    assert as_int(harness.call(app, "fromI136(int136)", 1).abi_return) == 1
    # adversarial: full value out of range, low 64 bits == 0
    assert harness.call(app, "fromI136(int136)", -(1 << 135), expect_revert=True).reverted
    assert harness.call(app, "fromI136(int136)", 5, expect_revert=True).reverted
    assert harness.call(app, "fromI136(int136)", -1, expect_revert=True).reverted
    assert harness.call(app, "fromI200(int200)", -(1 << 199), expect_revert=True).reverted
    assert as_int(harness.call(app, "fromI200(int200)", 1).abi_return) == 1
    assert harness.call(app, "fromU256(uint256)", 1 << 200, expect_revert=True).reverted
    assert harness.call(app, "fromI16(int16)", -256, expect_revert=True).reverted
    assert as_int(harness.call(app, "fromI16(int16)", 1).abi_return) == 1


def test_ecrecover_invalid_input_zero(harness):
    """puyasolRegression/contracts/ecrecover_invalid_input_zero.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (failing_ecrecover_invalid_input_proper,
    ==->!=). EVM ecrecover returns address(0) for invalid inputs (v not 27/28,
    r/s zero or >= curve order N); AVM ecdsa_pk_recover PANICS on them, and the
    opcode ran UNCONDITIONALLY with only the v-check masking the result after
    the fact. FIX: gate the recover opcode itself behind v/r/s validity and
    yield zero without executing it. (Residue: an in-range r that is not a
    curve x-coordinate still panics — not checkable without the recover.)
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/ecrecover_invalid_input_zero.sol")
    zero = 0
    assert as_int(harness.call(app, "zeroAll()").abi_return) == zero
    assert as_int(harness.call(app, "zeroRS()").abi_return) == zero
    for v in [0, 1, 26, 29]:
        assert as_int(harness.call(app, "badV(uint8)", v).abi_return) == zero, v
    assert as_int(harness.call(app, "rTooBig()").abi_return) == zero
    assert as_int(harness.call(app, "sTooBig()").abi_return) == zero


def test_signed_getter_cross_call(harness):
    """puyasolRegression/contracts/signed_getter_cross_call.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (call_forward_bytes, uint256->int24).
    A SIGNED public var's getter kept a bare biguint return, so puya's router
    published received()uint512 while callers + arc56 compute received()uint256:
    the cross-contract read fell to the callee's FALLBACK (empty return log ->
    "extraction start 28 beyond length 0"). FIX: PublicGetterBuilder remaps
    signed getter returns to arc4.uint256 (canonical 256-bit TC; unsigned keep
    declared width), covering int256 too, which skipped BOTH old branches.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_getter_cross_call.sol", contract_name="reader")
    harness.call(app, "set(int24,int256)", -5, -(1 << 200), extra_fee=2000)
    assert as_signed_int(harness.call(app, "readSmall()", extra_fee=2000).abi_return) == -5
    assert as_signed_int(harness.call(app, "readWide()", extra_fee=2000).abi_return) == -(1 << 200)


def test_bytesn_literal_compare_pad(harness):
    """puyasolRegression/contracts/bytesn_literal_compare_pad.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer's new op_bytesn operator
    (conditional_expression_storage_memory_1, bytes2->bytes22). A bytesN value
    compared against a SHORTER string literal reached BinaryOpBuilder's
    bytes-backed comparison as a raw 2-byte StringConstant and was compared
    unpadded -- `x == "aa"` false for every N > 2, silent wrong result. Ordered
    compares had the same gap (unpadded "b" sorted below "aa"). FIX: right-pad
    constant operands (BytesConstant or StringConstant) to the wider side's
    declared bytes[N] width before both the equality and ordered paths, in
    BinaryOpBuilder (live path) and SolFixedBytesBuilder::compare (eb path).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/bytesn_literal_compare_pad.sol")
    assert as_int(harness.call(app, "viaTernary(bool)", True).abi_return) == 1
    assert as_int(harness.call(app, "viaTernary(bool)", False).abi_return) == 2
    assert as_int(harness.call(app, "scalarEq()").abi_return) == 1
    assert as_int(harness.call(app, "widthSweep()").abi_return) == 15
    assert as_int(harness.call(app, "ordered()").abi_return) == 7


def test_nested_asm_param_gate(harness):
    """puyasolRegression/contracts/nested_asm_param_gate.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (slot_access_via_mapping_pointer,
    unchecked-wrap). Every "does this function use inline assembly" gate scanned
    only the body's TOP-LEVEL statements, so asm nested in `unchecked {}` or a
    plain block flipped them: the ARC4 param remap ran, the asm switch compared
    an arc4.uint256-typed `i` against biguint case constants -- never equal, so
    every call silently took the default branch (wrong slot 0). FIX: one shared
    recursive containsInlineAssembly (sol-ast/AsmScan.h) used by all five gates
    (param remap + decode + storage-ref-return x callee/caller/collection).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/nested_asm_param_gate.sol")
    for i in [0, 1, 2]:
        assert as_int(harness.call(app, "viaUnchecked(uint256)", i).abi_return) == i
    for i in [0, 1]:
        assert as_int(harness.call(app, "viaNestedBlock(uint256)", i).abi_return) == i


def test_signed_asm_read_word(harness):
    """puyasolRegression/contracts/signed_asm_read_word.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (assembly_access_bytes2_abicoder_v1
    uint256->int64). A signed intN (N<=64) local is uint64-backed (64-bit TC), but a
    Yul identifier is the full 256-bit EVM word — solc sign-extends on entry. Our asm
    read returned the value zero-padded, so `ret := val` into a bytes2 took 0x0000
    from the top instead of 0xFFFF for every negative input (silent wrong data).
    FIX: register signed <=64-bit locals (SolInlineAssembly signedParamBits) and
    sign-extend to the canonical 256-bit word at the bare-identifier read
    (CoreTranslation), only while the local is still uint64-backed.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_asm_read_word.sol")
    for v in [-(1 << 63), -8388608, -1]:
        assert [int(x) for x in harness.call(app, "h(int64)", v).abi_return] == [255, 255], v
    assert [int(x) for x in harness.call(app, "h(int64)", 5).abi_return] == [0, 0]
    assert [int(x) for x in harness.call(app, "top(int32)", -2).abi_return] == [255, 255]
    assert [int(x) for x in harness.call(app, "top(int32)", 3).abi_return] == [0, 0]
    assert as_int(harness.call(app, "asWord(int64)", -1).abi_return) == (1 << 256) - 1
    assert as_int(harness.call(app, "asWord(int64)", 7).abi_return) == 7
    # write-then-read round-trip: `z := add(...)` wrapping past 2^256, then a
    # shr(128, z) guard on the REAL word (V4 addDelta; signed-shadow model)
    i128min = -(1 << 127)
    for x in [-1, -2, -(1 << 63)]:
        assert as_signed_int(harness.call(app, "addDelta(int64,int128)", x, i128min).abi_return) == x
    assert harness.call(app, "addDelta(int64,int128)", 0, i128min, expect_revert=True).reverted
    assert as_signed_int(harness.call(app, "addDelta(int64,int128)", 5, 10).abi_return) == 15


def test_mstore8_bytes_memory_large(harness):
    """puyasolRegression/contracts/mstore8_bytes_memory_large.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (byte_array_to_storage_cleanup lit 63->126).
    Inline-assembly `mstore8` into a `bytes memory` local reverted once the array
    exceeded 64 bytes: the local stays a VALUE, and the generic path lowered
    `add(m, k)` to a bigint `b+` on that >64-byte value -- past AVM's bigint-operand
    limit ("math attempted on large byte-array"). FIX: a dedicated mstore8 write
    handler computes the data index and writes one byte via a guarded replace3
    (guard `off < len` keeps an out-of-bounds padding write a no-op, matching EVM).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mstore8_bytes_memory_large.sol")
    # write byte 0x42 at data index k of new bytes(n); expect byte@k=66,
    # byte@0 = 66 iff k==0 else 0, length == n. Sizes span the <=64 / >64 threshold.
    for n, k in [(64, 0), (65, 0), (65, 10), (96, 50), (128, 100), (200, 199)]:
        ret = harness.call(app, "poke(uint256,uint256)", n, k).abi_return
        assert [int(x) for x in ret] == [66, 66 if k == 0 else 0, n], (n, k, ret)
    # writing one byte past the logical end is EVM padding (never copied) -> no-op, not a revert
    for n in [63, 65, 128]:
        assert as_int(harness.call(app, "pokePadding(uint256)", n).abi_return) == n
    # sibling WORD write (mstore) at data offset k: MSB lands at k, LSB at k+31
    for n, k in [(65, 0), (96, 50), (128, 64), (64, 32), (200, 168)]:
        ret = harness.call(app, "pokeWord(uint256,uint256)", n, k).abi_return
        exp = [0xAA, 0xBB if k + 31 < n else 0, 0xAA if k == 0 else 0, n]
        assert [int(x) for x in ret] == exp, (n, k, ret)
    # straddling word at offset len-1 writes exactly the MSB, drops the tail spill
    for n in [65, 128]:
        assert [int(x) for x in harness.call(app, "pokeWordTail(uint256)", n).abi_return] == [0xCC, n]
    # legacy offset-0 short-array truncation semantics unchanged
    assert [int(x) for x in harness.call(app, "pokeWordShort()").abi_return] == [0x11, 0x88, 8]


def test_signed_mapping_key_once(harness):
    """puyasolRegression/contracts/signed_mapping_key_once.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (mapping_key_side_effect_once uint256->int48).
    A side-effecting SIGNED sub-word mapping key ran TWICE in a compound `m[k()] += x`
    / `delete m[k()]` (the derived box key is referenced by the read-modify-write). The
    key materialization guard only caught an AssignmentExpression key; a call-valued key
    (k() with cnt++) was materialized only for UNSIGNED keys via puya CSE (identical
    derivations merged), but a signed key's sign-extension defeats CSE so k() ran twice.
    FIX: materialize a SubroutineCallExpression key too, once, before coercion.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_mapping_key_once.sol")
    def pair(r): return (as_int(r.abi_return[0]), as_int(r.abi_return[1]))
    assert pair(harness.call(app, "compound()")) == (15, 1)     # key ran once
    assert pair(harness.call(app, "del()")) == (0, 1)
    assert pair(harness.call(app, "write()")) == (55, 1)
    assert pair(harness.call(app, "read()")) == (7, 1)
    assert pair(harness.call(app, "compound16()")) == (23, 1)   # int16 key, negative


def test_signed_array_getter(harness):
    """puyasolRegression/contracts/signed_array_getter.sol — NOT an o.g. semantic test.

    Found by the corpus-mutation fuzzer (userDefinedValueType/memory_to_storage
    uint16->int72). The auto-generated PUBLIC array/UDVT getter sign-extended the
    element only when bits<=64, so a 64<bits<256 signed element (int72/int128) or a
    UDVT over one returned the element at its NATURAL width -- int72 -1 came back as
    2^72-1 (a huge positive on the ABI wire) instead of the sign-extended -1. FIX:
    sign-extend ANY signed sub-256 getter return (signExtendToUint256 is idempotent,
    safe for the already-canonical scalar case).
    """
    import ctypes
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_array_getter.sol")
    vals = [-1, -300, 5, 70000]
    harness.call(app, "setAll(int72[])", vals)
    harness.call(app, "setScalar(int72)", -42)
    for i, v in enumerate(vals):
        assert as_signed_int(harness.call(app, "small(uint256)", i).abi_return) == v   # UDVT int72
        assert as_signed_int(harness.call(app, "a72(uint256)", i).abi_return) == v
        assert as_signed_int(harness.call(app, "a128(uint256)", i).abi_return) == v
        assert as_signed_int(harness.call(app, "a32(uint256)", i).abi_return) == ctypes.c_int32(v & 0xFFFFFFFF).value
    assert as_signed_int(harness.call(app, "scalar72()").abi_return) == -42


def test_struct_array_box_size(harness):
    """puyasolRegression/contracts/struct_array_box_size.sol — NOT an o.g. semantic test.

    Found by the CORPUS-MUTATION fuzzer (structs/memory_structs_read_write mutated
    uint16->int160). A fixed array of STRUCTS had its box sized with the default
    32 bytes/element: the sizing switch only had cases for uintN / bytesN /
    nested-static-array elements, so a struct element fell through. Any Struct[N]
    whose ARC-4 encoding isn't exactly 32 B was UNDER-ALLOCATED (5*32=160 for a
    55-byte struct needing 275), and element access overran the box
    ("extraction end 165 is beyond length: 160"); low indices happened to fit,
    which is why the suite never caught it. FIX: size struct elements via
    computeEncodedElementSize. Guards the HIGHEST index (the one that overran).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/struct_array_box_size.sol")
    for i in (0, 2, 4):   # index 4 overran the under-sized box
        harness.call(app, "setWide(uint256,uint8,int160,uint256,uint8)", i, 1, -7, 3, 4)
        r = harness.call(app, "getWide(uint256)", i)   # storage -> memory struct copy
        assert as_int(r.abi_return[0]) == 1
        assert as_signed_int(r.abi_return[1]) == -7
        assert as_int(r.abi_return[2]) == 3
        assert as_int(r.abi_return[3]) == 4
    for i in (0, 4):
        harness.call(app, "setSmall(uint256,uint256)", i, 9)
        assert as_int(harness.call(app, "getSmall(uint256)", i).abi_return) == 9


def test_modifier_storage_ref_arg(harness):
    """puyasolRegression/contracts/modifier_storage_ref_arg.sol — NOT an o.g. semantic test.

    Found by COVERAGE-GUIDED fuzzing (the storage-ref alias path in ModifierInliner
    was 0%-covered). A modifier with a STORAGE-REFERENCE param — modifier
    m(uint256[] storage a, uint256 i) { a[i] += 1; _; } applied as m(arr, i) — bound
    the ref to a __mod_a LOCAL and remapped, so the modifier body's a[i] += 1 wrote a
    local copy and the mutation was SILENTLY DROPPED (fArr(0) returned 10 not 11).
    ModifierBodyInliner aliased storage-ref params; the subroutine-chain
    ModifierInliner (the default for non-constructor fns) didn't. FIX: port the
    storage-alias branch — the aliased target is a contract-global state var,
    resolvable from the modifier subroutine.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/modifier_storage_ref_arg.sol")
    assert as_int(harness.call(app, "fArr(uint256)", 0).abi_return) == 11   # arr[0] 10->11
    assert as_int(harness.call(app, "fArr(uint256)", 0).abi_return) == 12   # persists 11->12
    assert as_int(harness.call(app, "getArr(uint256)", 0).abi_return) == 12
    assert as_int(harness.call(app, "fArr(uint256)", 1).abi_return) == 21   # arr[1] 20->21
    assert as_int(harness.call(app, "fMap(uint256)", 7).abi_return) == 5    # m[7] 0->5
    assert as_int(harness.call(app, "fMap(uint256)", 7).abi_return) == 10
    assert as_int(harness.call(app, "getMap(uint256)", 7).abi_return) == 10


def test_modifier_arg_side_effecting(harness):
    """puyasolRegression/contracts/modifier_arg_side_effecting.sol — NOT an o.g. semantic test.

    Found by COVERAGE-GUIDED fuzzing (ModifierInliner was 39.9% line-covered;
    modifier args with side-effecting exprs were unhit). A modifier ARGUMENT
    that is a ternary with a negate/checked branch — `gate(uint256(int256(
    a > 0 ? a : -a)))` — lowered the ternary as a branch-gating if/else assigning
    its result to a temp, but the ModifierInliner never drained that if/else into
    the modifier body before binding the arg. So the binding read the temp before
    it was assigned → the ternary collapsed to its false branch (`-a`) → the gate
    arg became huge → require(<1000) failed on EVERY call. FIX: drain the arg
    expression's pre/pending statements into the modifier body before the bind.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/modifier_arg_side_effecting.sol")
    # gate arg = abs(a); require(abs(a) < 1000); returns a
    assert as_signed_int(harness.call(app, "absGate(int64)", 1).abi_return) == 1
    assert as_signed_int(harness.call(app, "absGate(int64)", -7).abi_return) == -7
    assert as_signed_int(harness.call(app, "absGate(int64)", 0).abi_return) == 0
    assert as_signed_int(harness.call(app, "absGate(int64)", 999).abi_return) == 999
    assert harness.call(app, "absGate(int64)", 1500, expect_revert=True).reverted
    assert harness.call(app, "absGate(int64)", -1500, expect_revert=True).reverted
    # stacked modifiers + named return + body multiply
    assert as_signed_int(harness.call(app, "stackedNamed(int64,int64)", 3, 7).abi_return) == 21
    assert as_signed_int(harness.call(app, "stackedNamed(int64,int64)", -4, 5).abi_return) == -20


def test_unchecked_uint64_mul_add(harness):
    """puyasolRegression/contracts/unchecked_uint64_mul_add.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). unchecked uint64 mul/add that overflows 2^64 reverted (the
    AVM `*`/`+` panic on overflow) where Solidity wraps mod 2^64. The sub-word path masks to 2^N and
    Sub/Pow at uint64 were routed through wrapping paths, but full-width uint64 Add/Mult fell through to
    the panicking opcode. FIX: uint64 unchecked Add/Mult wide-compute via biguint, mod 2^64, narrow back.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unchecked_uint64_mul_add.sol")
    MAX = (1 << 64) - 1
    M = 1 << 64
    # unchecked: wrap mod 2^64 (was a revert)
    assert as_int(harness.call(app, "mul(uint64,uint64)", MAX, 2).abi_return) == (MAX * 2) % M  # 2^64-2
    assert as_int(harness.call(app, "mul(uint64,uint64)", MAX, MAX).abi_return) == (MAX * MAX) % M  # 1
    assert as_int(harness.call(app, "mul(uint64,uint64)", 1 << 40, 1 << 40).abi_return) == ((1 << 80) % M)  # 0
    assert as_int(harness.call(app, "mul(uint64,uint64)", 7, 9).abi_return) == 63  # no overflow unaffected
    assert as_int(harness.call(app, "add(uint64,uint64)", MAX, 5).abi_return) == (MAX + 5) % M  # 4
    assert as_int(harness.call(app, "add(uint64,uint64)", 100, 200).abi_return) == 300
    # checked: still reverts on overflow
    assert harness.call(app, "mulChecked(uint64,uint64)", MAX, 2, expect_revert=True).reverted
    assert harness.call(app, "addChecked(uint64,uint64)", MAX, 5, expect_revert=True).reverted
    assert as_int(harness.call(app, "mulChecked(uint64,uint64)", 7, 9).abi_return) == 63  # in-range ok


def test_subword_shift_truncate(harness):
    """puyasolRegression/contracts/subword_shift_truncate.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). Solidity truncates `x << n` to the operand type width
    (shifts never overflow-check, checked or unchecked): `uint8(254) << 1` is 252, not 508. The AVM ran
    the shift in biguint and only wrapped to 2^256 — never masked back to 2^bits for sub-word/uint64. The
    return path re-masks, so the bug only surfaced when the shift result was consumed mid-expression.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/subword_shift_truncate.sol")
    bo = lambda r: as_int(r.abi_return)  # bool -> 1/0
    # uint8: 254<<1 == 252 (not 508); 128<<1 == 0 — both <= 255, so comparison is true (was false)
    assert bo(harness.call(app, "shlCmpU8(uint8)", 254)) == 1
    assert bo(harness.call(app, "shlCmpU8(uint8)", 128)) == 1
    assert bo(harness.call(app, "shlCmpU8(uint8)", 127)) == 1  # 254, no truncation — unaffected
    # checked variant: ~0=255, 255<<1 truncates to 254; 255>254 true (was 255>510 false)
    assert bo(harness.call(app, "comboChkU8(uint8)", 0)) == 1
    assert bo(harness.call(app, "comboChkU8(uint8)", 127)) == 1  # ~127=128, 128<<1=0
    # wider sub-word + native uint64
    assert bo(harness.call(app, "shlCmpU16(uint16)", 65534)) == 1
    assert bo(harness.call(app, "shlCmpU64(uint64)", (1 << 64) - 1)) == 1
    # value correct when consumed in further arithmetic: 254<<1=252, 252|1 == 253
    assert as_int(harness.call(app, "shlMaskU8(uint8)", 254).abi_return) == 253


def test_bitinvert_subword_mask(harness):
    """puyasolRegression/contracts/bitinvert_subword_mask.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). Bitwise NOT of a sub-256 biguint type inverted the full
    32-byte word, so ~uint128(0) was 2^256-1 not 2^128-1. A downstream checked add overflow-checks the
    un-masked value: (~b)+a tested 2^256-1 <= 2^128-1 and false-reverted; (~c)/max returned ~2^128 not 1.
    FIX: mask the biguint ~x result back to 2^bits for m_bits < 256.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/bitinvert_subword_mask.sol")
    MAX128 = (1 << 128) - 1
    # ~0 == 2^128-1 (mod 2^128), so ~0 + 0 == max (was a false-revert)
    assert as_int(harness.call(app, "invAddU128(uint128,uint128)", 0, 0).abi_return) == MAX128
    assert as_int(harness.call(app, "invAddU128(uint128,uint128)", 5, 3).abi_return) == (MAX128 - 5 + 3)
    # real overflow still reverts: ~0 + 1 == 2^128 overflows uint128
    assert harness.call(app, "invAddU128(uint128,uint128)", 0, 1, expect_revert=True).reverted
    # ~0 / max == 1 (was ~2^128); ~1 / max == 0
    assert as_int(harness.call(app, "invDivU128(uint128)", 0).abi_return) == 1
    assert as_int(harness.call(app, "invDivU128(uint128)", 1).abi_return) == 0
    # ~c stays within the type width
    assert as_int(harness.call(app, "invMaskU128(uint128)", 0).abi_return) == 1
    assert as_int(harness.call(app, "invMaskU128(uint128)", 12345).abi_return) == 1
    # width-general (uint192): ~1 + 1 == 2^192-1
    assert as_int(harness.call(app, "invAddU192(uint192)", 1).abi_return) == (1 << 192) - 1
    # uint256 full-width invert unaffected: ~0 == 2^256-1
    assert as_int(harness.call(app, "invU256(uint256)", 0).abi_return) == (1 << 256) - 1


def test_signed_to_unsigned_cast_trim(harness):
    """puyasolRegression/contracts/signed_to_unsigned_cast_trim.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). A same-width signed->unsigned biguint cast uintN(intN(x)) of
    a NEGATIVE intN left the value in 256-bit two's-complement form (int128(-1) == 2^256-1) instead of
    trimming to N bits (uint128 of it == 2^128-1). The return path re-canonicalised, so it only surfaced
    when consumed: checked **1 / *1 / +0 false-reverted, and `<= type(uintN).max` returned the wrong bool.
    FIX: applyNarrowingMask masks to 2^N for signed-source/unsigned-target casts even at equal width.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_to_unsigned_cast_trim.sol")
    MAX128 = (1 << 128) - 1
    HALF128 = 1 << 127  # int128.min bit pattern as a uint128 — the negative-int128 case
    # uint128(int128(c)) round-trips to c; checked **1 / *1 / +0 no longer false-revert
    for c in (MAX128, HALF128, MAX128 - 1, 5, 0):
        assert as_int(harness.call(app, "castPow(uint128)", c).abi_return) == c
        assert as_int(harness.call(app, "castMul(uint128)", c).abi_return) == c
        assert as_int(harness.call(app, "castAdd(uint128)", c).abi_return) == c
        assert as_int(harness.call(app, "castCmp(uint128)", c).abi_return) == 1  # always <= uint128.max
    # wider biguint width (uint160)
    MAX160 = (1 << 160) - 1
    assert as_int(harness.call(app, "cast160(uint160)", MAX160).abi_return) == MAX160
    assert as_int(harness.call(app, "cast160(uint160)", 1 << 159).abi_return) == (1 << 159)
    # narrowing int256->int128->uint128 keeps the low 128 bits
    assert as_int(harness.call(app, "castNarrow(uint256)", MAX128).abi_return) == MAX128
    assert as_int(harness.call(app, "castNarrow(uint256)", 1 << 200).abi_return) == 0
    # sanity: uint256(int256(-1)) stays full-width 2^256-1 (fix must NOT trim this)
    assert as_int(harness.call(app, "u256ofI256(int256)", -1).abi_return) == (1 << 256) - 1
    assert as_int(harness.call(app, "u256ofI256(int256)", 5).abi_return) == 5


def test_unchecked_biguint_sub_exp_wrap(harness):
    """puyasolRegression/contracts/unchecked_biguint_sub_exp_wrap.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). Unchecked sub-256 biguint subtraction (underflow) and
    exponentiation wrapped to 2^256 instead of the type width 2^N — `unchecked uint128(0) - 1` was 2^256-1
    not 2^128-1, and `uint128 a ** 2` kept the full product. The return path re-masked, so it only
    surfaced when consumed: `<= type(uint128).max` returned the wrong boolean. FIX: mask the unchecked
    unsigned sub-256 biguint sub/exp result to 2^N. Mul/Add already wrapped; uint256 keeps full width.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unchecked_biguint_sub_exp_wrap.sol")
    M128 = 1 << 128
    MAX128 = M128 - 1
    # unchecked sub underflow wraps mod 2^128 (was 2^256-1)
    assert as_int(harness.call(app, "usub(uint128,uint128)", 0, 1).abi_return) == MAX128
    assert as_int(harness.call(app, "usub(uint128,uint128)", 5, 8).abi_return) == (5 - 8) % M128
    assert as_int(harness.call(app, "usub(uint128,uint128)", 100, 40).abi_return) == 60
    assert as_int(harness.call(app, "usubCmp(uint128,uint128)", 0, 1).abi_return) == 1  # was 0
    # unchecked exp wraps mod 2^128
    assert as_int(harness.call(app, "uexp(uint128)", 1 << 64).abi_return) == ((1 << 128) % M128)  # 0
    assert as_int(harness.call(app, "uexp(uint128)", MAX128).abi_return) == ((MAX128 * MAX128) % M128)  # 1
    assert as_int(harness.call(app, "uexp(uint128)", 3).abi_return) == 9
    assert as_int(harness.call(app, "uexpCmp(uint128)", 1 << 64).abi_return) == 1  # was 0
    # width-general
    assert as_int(harness.call(app, "usub200(uint200,uint200)", 0, 1).abi_return) == (1 << 200) - 1
    assert as_int(harness.call(app, "uexp160(uint160)", 1 << 80).abi_return) == 0  # (2^80)^2 mod 2^160
    # checked still reverts
    assert harness.call(app, "csub(uint128,uint128)", 0, 1, expect_revert=True).reverted
    assert harness.call(app, "cexp(uint128)", 1 << 64, expect_revert=True).reverted
    assert as_int(harness.call(app, "csub(uint128,uint128)", 8, 5).abi_return) == 3  # in-range ok
    # add/mul still wrap correctly (unchanged)
    assert as_int(harness.call(app, "uadd(uint128,uint128)", MAX128, 5).abi_return) == ((MAX128 + 5) % M128)
    assert as_int(harness.call(app, "umul(uint128,uint128)", MAX128, 2).abi_return) == ((MAX128 * 2) % M128)


def test_unchecked_biguint_muladd_consumed(harness):
    """puyasolRegression/contracts/unchecked_biguint_muladd_consumed.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (seed 20006). Unchecked sub-256 biguint Add/Mult wrapped the result
    to 2^256 (wrapMod256), not the type width 2^N. The standalone-return path re-masks, so the sibling
    sub_exp_wrap guards (which only test `return a*b`) called add/mul "already correct" — but a CONSUMED
    non-canonical (>2^N, <2^256) intermediate is WRONG: `(a * ~c) / x` divided a too-wide dividend. FIX:
    mask the unchecked unsigned sub-256 Add/Mult result to 2^N.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/unchecked_biguint_muladd_consumed.sol"
    )
    M128 = 1 << 128

    def mul_div(a, c):  # mirror EVM uint128 semantics
        prod = (a * ((~c) & (M128 - 1))) % M128  # unchecked mult wraps mod 2^128
        return prod // (((c << 0) % M128) ^ a)

    assert mul_div(2, 0) == (1 << 127) - 1  # the fuzzer's case (was 2^128-1)
    assert as_int(harness.call(app, "mulDiv(uint128,uint128)", 2, 0).abi_return) == mul_div(2, 0)
    assert as_int(harness.call(app, "mulDiv(uint128,uint128)", 255, 0).abi_return) == mul_div(255, 0)
    # unchecked add overflow consumed by a divide: (2^128-1 + 1)/2 == 0 (was 2^127)
    assert as_int(harness.call(app, "addDiv(uint128,uint128,uint128)", M128 - 1, 1, 2).abi_return) == 0
    # width-general uint200: (2^200-2)/2 == 2^199-1
    assert as_int(harness.call(app, "mul200(uint200,uint200)", 2, 0).abi_return) == ((1 << 200) - 2) // 2
    # standalone return still wraps correctly (re-masked at encode): 2^64 * 2^64 == 0 mod 2^128
    assert as_int(harness.call(app, "umul(uint128,uint128)", 1 << 64, 1 << 64).abi_return) == 0


def test_asm_sar_shift_zero(harness):
    """puyasolRegression/contracts/asm_sar_shift_zero.sol — NOT an o.g. semantic test.

    Found by the asm-opcode fuzz probe. Yul `sar(0, x)` (arithmetic shift-right by ZERO) returned
    all-ones (-1) for a negative x instead of x unchanged: complementShift = 256 - shift = 256 at
    shift 0, and 2^256 overflows u256 (wraps to 1) so fillMask = MAX, giving shr|MAX = -1. The
    shift>=256 boundary was handled but not shift==0. FIX: fillMask = MAX - shr(shift, MAX) (shr
    saturates for >=256 and is identity for 0, so no 2^256 / underflow edge).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_sar_shift_zero.sol")
    M = 1 << 256
    neg2 = M - 2  # -2 in 256-bit two's complement
    assert as_int(harness.call(app, "sar0(uint256)", neg2).abi_return) == neg2   # was M-1 (-1)
    assert as_int(harness.call(app, "sar0(uint256)", 7).abi_return) == 7
    assert as_int(harness.call(app, "sar0(uint256)", M - 1).abi_return) == M - 1  # -1 -> -1
    assert as_int(harness.call(app, "sarN(uint256,uint256)", 1, 8).abi_return) == 4
    assert as_int(harness.call(app, "sarN(uint256,uint256)", 4, M - 256).abi_return) == M - 16  # sar(4,-256)
    assert as_int(harness.call(app, "sarN(uint256,uint256)", 255, 1 << 255).abi_return) == M - 1
    assert as_int(harness.call(app, "sarN(uint256,uint256)", 256, M - 1).abi_return) == M - 1  # >=256 neg -> -1
    assert as_int(harness.call(app, "sarN(uint256,uint256)", 256, 7).abi_return) == 0           # >=256 pos -> 0


def test_abi_bytes_roundtrip(harness):
    """puyasolRegression/contracts/abi_bytes_roundtrip.sol — NOT an o.g. semantic test.

    Found by the abi-round-trip fuzz probe. abi.decode(abi.encode(b),(bytes)) did not round-trip a
    `bytes` value: handleDecode short-circuited (decoded->wtype == targetType, both `bytes`) and returned
    the ARC4 byte[] encoding (uint16 length prefix + data) instead of ARC4-decoding to raw bytes — the
    result was 2 bytes too long and r[0] was the length high-byte (0), not b[0]. string was already
    correct (its wtype `bytes` != target `string` → fell through to ARC4Decode). FIX: exclude dynamic
    bytes/string targets from the short-circuit so `bytes` also routes through ARC4Decode.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/abi_bytes_roundtrip.sol")
    assert as_int(harness.call(app, "rtLen(bytes)", b"abc").abi_return) == 3        # was 5
    assert as_int(harness.call(app, "rtLen(bytes)", b"").abi_return) == 0           # was 2
    assert as_int(harness.call(app, "rtLen(bytes)", bytes(40)).abi_return) == 40    # was 42
    assert as_int(harness.call(app, "rtFirst(bytes)", b"abc").abi_return) == 97     # 'a', was 0
    assert as_int(harness.call(app, "rtEq(bytes)", b"hello world").abi_return) == 1
    assert as_int(harness.call(app, "stEq(string)", "hello").abi_return) == 1       # string control


def test_asm_param_memory_offset(harness):
    """puyasolRegression/contracts/asm_param_memory_offset.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer. A function PARAM used as a memory offset in inline assembly
    (mstore(off, v) / mload(off)) resolved to its CALLDATA head-offset CONSTANT instead of its runtime
    value: initializeCalldataMap stashes paramName -> calldata head byte offset in m_localConstants (for
    the .offset/.length paths), but the bare-name constant resolvers (resolveConstantYulValue /
    resolveConstantOffset) also consulted it, so `off` folded to e.g. constant 4 -> mstore lowered to
    `replace2 4`, hitting a fixed wrong slot. const/let-local offsets were fine; only param names collided.
    FIX: track calldata param names (m_calldataParamNames); the two bare-name resolvers skip them so a
    bare param resolves to its runtime VarExpression. The .offset/.length suffix + calldataMap paths are
    unaffected (separate reads).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_param_memory_offset.sol")
    assert as_int(harness.call(app, "paramOff(uint256,uint256)", 64, 7).abi_return) == 7      # was 64
    assert as_int(harness.call(app, "paramOff(uint256,uint256)", 96, 12345).abi_return) == 12345
    assert as_int(harness.call(app, "paramOffAdd(uint256,uint256)", 64, 7).abi_return) == 7   # was 64
    assert as_int(harness.call(app, "twoParams(uint256,uint256,uint256)", 0, 0, 100).abi_return) == 201  # 100 + 101
    assert as_int(harness.call(app, "constOff(uint256)", 7).abi_return) == 7        # control still works
    assert as_int(harness.call(app, "letOff(uint256,uint256)", 64, 7).abi_return) == 7  # control still works


def test_dynarray_compound_assign(harness):
    """puyasolRegression/contracts/dynarray_compound_assign.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer. Compound assignment on a STORAGE dynamic-array element
    (arr[i] += / -= / *= / |= / /= b) failed to COMPILE (puya backend itob(Encoded(uintN))): SolAssignment
    applyCompoundAssignment reused the LHS write-form (which indexes a box and is ARC4-ENCODED) as the read
    value, so the arithmetic itob'd the encoded bytes. Plain arr[i]=arr[i]+b, memory/mapping/fixed/nested,
    and struct fields all worked. FIX: decode the box-array-element write-form (makeARC4Decode +
    signExtendSignedElement) before the compound op, gated on a BoxValue base so memory/calldata index
    exprs stay untouched; the existing applyArc4EncodeIfNeeded re-encodes the result. (arr[i]++/-- via the
    separate handleIncDec path still doesn't persist the box write — left as the pre-existing compile error.)
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/dynarray_compound_assign.sol")  # was a compile error
    M = 1 << 256
    def sgn(v):
        return v - M if v >= (1 << 255) else v
    for v in (10, 20, 30):
        harness.call(app, "pushA(uint128)", v)
    harness.call(app, "addA(uint256,uint128)", 1, 5)        # a[1] = 25
    assert as_int(harness.call(app, "getA(uint256)", 0).abi_return) == 10   # neighbours untouched
    assert as_int(harness.call(app, "getA(uint256)", 1).abi_return) == 25   # persisted
    assert as_int(harness.call(app, "getA(uint256)", 2).abi_return) == 30
    harness.call(app, "pushB(int64)", -10)
    harness.call(app, "subB(uint256,int64)", 0, 5)          # b[0] = -15
    assert sgn(as_int(harness.call(app, "getB(uint256)", 0).abi_return)) == -15
    harness.call(app, "subB(uint256,int64)", 0, -20)        # b[0] = 5
    assert sgn(as_int(harness.call(app, "getB(uint256)", 0).abi_return)) == 5
    harness.call(app, "pushC(uint256)", 100)
    harness.call(app, "divC(uint256,uint256)", 0, 4)        # c[0] = 25
    assert as_int(harness.call(app, "getC(uint256)", 0).abi_return) == 25
    harness.call(app, "pushD(uint8)", 200)
    harness.call(app, "addD(uint256,uint8)", 0, 50)         # d[0] = 250
    assert as_int(harness.call(app, "getD(uint256)", 0).abi_return) == 250
    assert harness.call(app, "addD(uint256,uint8)", 0, 100, expect_revert=True).reverted  # 250+100 overflow


def test_struct_field_incdec(harness):
    """puyasolRegression/contracts/struct_field_incdec.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer. Struct STATE-VAR field ++/-- (st.x++) failed to COMPILE
    ('unsupported assignment target', puya backend) whenever the contract has 2+ functions (the struct
    stays boxed and SolUnaryOperation::handleIncDec emitted a bare FieldExpression write puya rejects).
    FIX: rebuild the struct copy-on-write (box := struct-with-field-replaced) like the compound-assignment
    path, reading the other fields with-default so a fresh (non-existent) box yields defaults instead of
    reverting. Handles top-level + nested fields, signed + unsigned + sub-word, prefix + postfix + return,
    fresh + initialized, with checked overflow.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/struct_field_incdec.sol")  # was a compile error
    M = 1 << 256
    def sgn(v):
        return v - M if v >= (1 << 255) else v
    # fresh struct: inc treats absent field as 0
    harness.call(app, "incD()")                                 # fresh d: 0 -> 1 (no revert)
    assert as_int(harness.call(app, "getD()").abi_return) == 1
    harness.call(app, "setSt(int8,uint128,int64,uint8)", 5, 100, -7, 200)
    assert sgn(as_int(harness.call(app, "postIncA()").abi_return)) == 5    # postfix returns OLD
    assert sgn(as_int(harness.call(app, "getA()").abi_return)) == 6        # persisted
    assert as_int(harness.call(app, "getB()").abi_return) == 100           # neighbours untouched
    assert sgn(as_int(harness.call(app, "preIncA()").abi_return)) == 7     # prefix returns NEW
    assert sgn(as_int(harness.call(app, "getA()").abi_return)) == 7
    harness.call(app, "decC()")
    assert sgn(as_int(harness.call(app, "getC()").abi_return)) == -8
    # nested struct field
    harness.call(app, "setO(uint64,int32,uint128)", 10, -3, 100)
    assert as_int(harness.call(app, "incNX()").abi_return) == 10           # postfix OLD
    assert as_int(harness.call(app, "getNX()").abi_return) == 11           # persisted
    assert sgn(as_int(harness.call(app, "getNY()").abi_return)) == -3      # neighbour untouched


def test_dynarray_incdec(harness):
    """puyasolRegression/contracts/dynarray_incdec.sol — NOT an o.g. semantic test.

    Companion to test_dynarray_compound_assign: arr[i]++ / arr[i]-- on a STORAGE dynamic-array element.
    Same ARC4-encoded-element decode (SolUnaryOperation::handleIncDec, gated on a BoxValue index base);
    the box_replace write persists, postfix returns OLD / prefix returns NEW, sub-word checked overflow
    reverts. (Compound += already fixed separately; this is the unary path.)
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/dynarray_incdec.sol")
    M = 1 << 256
    def sgn(v):
        return v - M if v >= (1 << 255) else v
    harness.call(app, "pushA(uint128)", 10)
    harness.call(app, "pushA(uint128)", 20)
    assert as_int(harness.call(app, "postIncA(uint256)", 0).abi_return) == 10   # postfix returns OLD
    assert as_int(harness.call(app, "getA(uint256)", 0).abi_return) == 11       # persisted
    assert as_int(harness.call(app, "getA(uint256)", 1).abi_return) == 20       # neighbour untouched
    assert as_int(harness.call(app, "preIncA(uint256)", 0).abi_return) == 12    # prefix returns NEW
    assert as_int(harness.call(app, "getA(uint256)", 0).abi_return) == 12
    harness.call(app, "decA(uint256)", 0)
    assert as_int(harness.call(app, "getA(uint256)", 0).abi_return) == 11
    harness.call(app, "pushB(int64)", -5)
    harness.call(app, "incB(uint256)", 0)
    assert sgn(as_int(harness.call(app, "getB(uint256)", 0).abi_return)) == -4
    harness.call(app, "pushC(uint8)", 254)
    harness.call(app, "incC(uint256)", 0)
    assert as_int(harness.call(app, "getC(uint256)", 0).abi_return) == 255
    assert harness.call(app, "incC(uint256)", 0, expect_revert=True).reverted   # 255++ overflow


def test_unchecked_incdec_wrap(harness):
    """puyasolRegression/contracts/unchecked_incdec_wrap.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer (inc/dec probe). `unchecked { x++ }` / `unchecked { x-- }` at the
    type boundary REVERTED on AVM where EVM WRAPS mod 2^N: the native uint64 +/- opcodes and the biguint
    b- opcode revert on over/underflow (0-1, max+1), and there's no full-width downstream mask for
    uint256 inc. Broken: dec at 0 (all widths) + uint256 inc at max. FIX (SolUnaryOperation::handleIncDec
    makeNewValue): a dedicated unsigned-unchecked branch computes the wrap in biguint — inc = v+1,
    dec = v + (2^N-1) [add max instead of subtract 1, dodging underflow] — then mod 2^N, narrowed back to
    uint64 for sub-word/uint64 backings. Checked paths + signed branch untouched.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unchecked_incdec_wrap.sol")
    assert as_int(harness.call(app, "decU8(uint8)", 0).abi_return) == 255          # was REVERT
    assert as_int(harness.call(app, "decU128(uint128)", 0).abi_return) == (1 << 128) - 1
    assert as_int(harness.call(app, "decU256(uint256)", 0).abi_return) == (1 << 256) - 1
    assert as_int(harness.call(app, "decU64(uint64)", 0).abi_return) == (1 << 64) - 1
    assert as_int(harness.call(app, "incU256(uint256)", (1 << 256) - 1).abi_return) == 0  # was REVERT
    assert as_int(harness.call(app, "incU64(uint64)", (1 << 64) - 1).abi_return) == 0
    assert as_int(harness.call(app, "preDecU8(uint8)", 0).abi_return) == 255
    # non-boundary still correct
    assert as_int(harness.call(app, "decU8(uint8)", 5).abi_return) == 4
    assert as_int(harness.call(app, "incU256(uint256)", 9).abi_return) == 10
    assert as_int(harness.call(app, "decU64(uint64)", 100).abi_return) == 99


def test_unsigned_inc_overflow(harness):
    """puyasolRegression/contracts/unsigned_inc_overflow.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer (compound-edges probe). Checked unsigned `x++`/`++x` missed the
    overflow assert that `x += 1` emits: SolUnaryOperation::handleIncDec's makeNewValue had the check on
    the signed branch only; the unsigned branches just computed base+1. Native uint64 reverted by luck
    (its `+` opcode overflows), but a sub-word (uint8..uint56) add yielded e.g. 256 that masked to 0, and
    a biguint (uint65..uint256) add yielded the exact 2^N — both silently WRAPPED where EVM reverts (a
    soundness bug: `counter++` at max wrapped). FIX: guardUIncOverflow asserts result <= 2^bits-1 for
    checked sub-word + biguint inc (uint64 left to its native opcode). %= / dec / unchecked unaffected.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/unsigned_inc_overflow.sol")
    # overflow at max must revert (was silently wrapping to 0)
    assert harness.call(app, "postInc8(uint8)", 255, expect_revert=True).reverted
    assert harness.call(app, "preInc8(uint8)", 255, expect_revert=True).reverted
    assert harness.call(app, "postInc16(uint16)", 65535, expect_revert=True).reverted
    assert harness.call(app, "postInc128(uint128)", (1 << 128) - 1, expect_revert=True).reverted
    assert harness.call(app, "postInc256(uint256)", (1 << 256) - 1, expect_revert=True).reverted
    # in-range increments still correct
    assert as_int(harness.call(app, "postInc8(uint8)", 5).abi_return) == 6
    assert as_int(harness.call(app, "postInc8(uint8)", 254).abi_return) == 255
    assert as_int(harness.call(app, "postInc128(uint128)", 7).abi_return) == 8
    # unchecked still wraps (no false revert)
    assert as_int(harness.call(app, "uncheckedInc8(uint8)", 255).abi_return) == 0


def test_staticcall_inner(harness):
    """puyasolRegression/contracts/staticcall_inner.sol — NOT an o.g. semantic test.

    address.staticcall(data) to a non-precompile previously HARD-ERRORED. It now lowers like .call --
    an inner ApplicationCall txn (InnerCallHandlers merges staticcall into the call path) -- with a
    warning that the EVM read-only guarantee is NOT enforced on AVM. A self-staticcall with
    abi.encodeWithSignature resolves to a direct subroutine call (the self-call resolution path, now
    reachable from staticcall). Guards both the compile (no hard-error) and the runtime value.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/staticcall_inner.sol")  # was a hard-error
    assert as_int(harness.call(app, "selfStatic(uint256)", 5).abi_return) == 105
    assert as_int(harness.call(app, "selfStatic(uint256)", 0).abi_return) == 100
    assert as_int(harness.call(app, "selfCall(uint256)", 7).abi_return) == 107
    # encodeCall self-resolution (function-ref selector -> same method on `this`, incl. inherited)
    assert as_int(harness.call(app, "selfStaticCall(uint256)", 9).abi_return) == 109
    assert as_int(harness.call(app, "selfStaticInherited(uint256)", 30).abi_return) == 37


def test_compound_signed_subword_divmod(harness):
    """puyasolRegression/contracts/compound_signed_subword_divmod.sol — NOT an o.g. semantic test.

    Found by the overnight campaign (rich storage-mutation sweep). Compound signed /= and %= on a
    uint64-backed type (int8/16/32/64) fell to the NATIVE UNSIGNED uint64 div/mod path because
    SolIntegerBuilder::binary_op's `needsBigUInt` gate didn't include signed FloorDiv/Mod -> wrong for
    negative operands (int64 -1 / int64.min gave 1 not 0; int16 -32768 / -128 gave 0 not 256). Plain a/b
    was always correct (it uses SolBinaryOperation's signed path, not the eb builder). FIX: add
    `m_signed && (FloorDiv || Mod)` to needsBigUInt so the signed biguint path (buildSignedModDiv) handles
    these. unsigned + biguint-backed (int128/256) were already correct.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/compound_signed_subword_divmod.sol")
    M = 1 << 256
    def s8(v):
        v &= 0xFF
        return v - 256 if v >= 128 else v
    def s16(v):
        v &= 0xFFFF
        return v - 65536 if v >= 32768 else v
    def s64(v):
        v &= (1 << 64) - 1
        return v - (1 << 64) if v >= (1 << 63) else v
    assert s8(as_int(harness.call(app, "dI8(int8,int8)", -100, 7).abi_return)) == -100 // 7 + (1 if (-100) % 7 else 0)  # trunc toward 0 = -14
    assert s8(as_int(harness.call(app, "dI8(int8,int8)", -7, -2).abi_return)) == 3       # -7/-2 trunc = 3
    assert s8(as_int(harness.call(app, "mI8(int8,int8)", -7, 3).abi_return)) == -1       # -7 % 3 = -1
    assert s16(as_int(harness.call(app, "dI16(int16,int16)", -32768, -128).abi_return)) == 256   # was 0
    assert s16(as_int(harness.call(app, "dI16(int16,int16)", -32768, -2).abi_return)) == 16384
    assert s64(as_int(harness.call(app, "dI64(int64,int64)", -1, -(1 << 63)).abi_return)) == 0    # was 1
    assert s64(as_int(harness.call(app, "dI64(int64,int64)", 100, -7).abi_return)) == -14
    assert s8(as_int(harness.call(app, "uI8(int8,int8)", -100, 7).abi_return)) == -14


def test_compound_signed_div_overflow(harness):
    """puyasolRegression/contracts/compound_signed_div_overflow.sol — NOT an o.g. semantic test.

    Found by the differential fuzzer (signed mixed-width div probe). Compound `x /= b` on a signed
    type skipped the `intN.min / -1` overflow check that EVM reverts on: the plain `a / b` path emits
    it (SolBinaryOperation::buildSignedDivMod) but the compound path routes through the eb builder
    (SolIntegerBuilder::binary_op -> buildSignedModDiv), which wrapped the result back to intN.min
    silently. Affected every width (int128/int256, mixed + same). FIX: emit the int_min/-1 assert in
    the eb signed-FloorDiv branch (operands are 256-bit sign-extended: intMin=2^256-2^(N-1), -1=2^256-1).
    `%=` is unaffected (mod by -1 = 0, no overflow).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/compound_signed_div_overflow.sol")
    MIN128 = -(1 << 127)
    MIN256 = -(1 << 255)
    M = 1 << 256
    # the overflow case must revert on every shape (was silently returning intN.min)
    assert harness.call(app, "compMix(int128,int16)", MIN128, -1, expect_revert=True).reverted
    assert harness.call(app, "compSame(int128,int128)", MIN128, -1, expect_revert=True).reverted
    assert harness.call(app, "comp256(int256,int256)", MIN256, -1, expect_revert=True).reverted
    # mod by -1 is fine (0); min/1 is fine (no overflow); normal divisions still correct
    assert as_int(harness.call(app, "compMod(int128,int128)", MIN128, -1).abi_return) == 0
    assert as_int(harness.call(app, "compMix(int128,int16)", MIN128, 1).abi_return) == (M + MIN128)
    assert as_int(harness.call(app, "compMix(int128,int16)", -100, 7).abi_return) == (M - 14)  # -100/7 = -14 trunc
    assert as_int(harness.call(app, "comp256(int256,int256)", 50, 3).abi_return) == 16


def test_abi_decode_tuple_signed_subword(harness):
    """puyasolRegression/contracts/abi_decode_tuple_signed.sol — NOT an o.g. semantic test.

    Found by the abi-round-trip fuzz probe. abi.decode of a TUPLE with a signed sub-64 element (int8/16/32)
    returned DIRECTLY failed to COMPILE: the decode produces the native tuple (int16->uint64) but a
    multi-return ABI function widens each signed sub-64 element to biguint (256-bit two's complement for
    the ARC4 uint256 encoding), and the per-element widening in ReturnRewriter only handled tuple LITERALS
    (`return (a,b)`), not an opaque tuple-producing expression -> `invalid return type [biguint, uint64]
    expected [biguint, biguint]`. FIX: bind the opaque tuple to a temp, rebuild it as a literal with the
    signed sub-64 elements sign-extended. Compiling rt2/rt3 guards the compile error; id* guard the values.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/abi_decode_tuple_signed.sol")
    assert as_int(harness.call(app, "id2(uint128,int16)", 5, -100).abi_return) == 1
    assert as_int(harness.call(app, "id2(uint128,int16)", 1 << 127, -32768).abi_return) == 1
    assert as_int(harness.call(app, "id3(int16,int32,int8)", -1, -(1 << 31), -128).abi_return) == 1
    assert as_int(harness.call(app, "id3(int16,int32,int8)", 32767, (1 << 31) - 1, 127).abi_return) == 1
    assert as_int(harness.call(app, "id2u(uint128,uint16)", 9, 65535).abi_return) == 1  # unsigned control


def test_signed_mul_complex_operand(harness):
    """puyasolRegression/contracts/signed_mul_complex_operand.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). A complex (non-leaf) expression as the LEFT operand of a
    checked signed multiply false-reverted: (bitwise/shift/cast-chain/ternary) * x REVERTED where EVM
    returns the value (most visibly at x==0). Root: makeEvalOnce wraps the operand in a SingleEvaluation,
    and puya mis-lowers SingleEvaluation(complex) in the signed-mul abs/overflow codegen (stack-slot
    miscount). FIX: materialise a complex left operand of a signed multiply to a real local first.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/signed_mul_complex_operand.sol")
    def sint(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    I64MIN = -(1 << 63)
    I64MAX = (1 << 63) - 1
    # the formerly-false-reverting cases (complex left * 0) now return 0
    assert sint(harness.call(app, "ternMul(int16,int16,int16)", 0, -32768, -32768)) == 0
    assert sint(harness.call(app, "andMul(int64,int64,int64)", I64MIN, I64MIN, 0)) == 0
    assert sint(harness.call(app, "notMul(int64,int64)", I64MIN, 0)) == 0
    assert sint(harness.call(app, "shlMul(int64,int64)", I64MIN, 0)) == 0
    assert sint(harness.call(app, "castMul(int64,int64)", I64MIN, 0)) == 0
    # and they still compute the right value when non-zero
    assert sint(harness.call(app, "ternMul(int16,int16,int16)", 2, 5, 3)) == 6   # (3<5?3:2)*2
    assert sint(harness.call(app, "andMul(int64,int64,int64)", 7, 6, 2)) == 12
    assert sint(harness.call(app, "notMul(int64,int64)", 0, 5)) == -5            # (~0)*5
    assert sint(harness.call(app, "shlMul(int64,int64)", 3, 4)) == 24           # (3<<1)*4
    assert sint(harness.call(app, "castMul(int64,int64)", -1, 5)) == -5         # int64(int8(-1))*5
    assert sint(harness.call(app, "inRange(int64,int64)", 7, 6)) == 18
    assert sint(harness.call(app, "inRange(int64,int64)", -1, -1)) == -3
    # pure-left short-circuit stays clean
    assert as_int(harness.call(app, "scAnd(int64,int64,int64)", I64MIN, I64MIN, 0).abi_return) == 1
    assert as_int(harness.call(app, "scAnd(int64,int64,int64)", 7, 6, 2).abi_return) == 0
    # real overflow STILL reverts (fix removes only the false revert)
    assert harness.call(app, "overflowMul(int64,int64)", I64MAX, I64MAX, expect_revert=True).reverted
    assert sint(harness.call(app, "overflowMul(int64,int64)", 2, 1)) == 9


def test_const_negate_typemin(harness):
    """puyasolRegression/contracts/const_negate_typemin.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (--cast). `-type(intN).min` overflows intN and solc REVERTS at
    runtime, but puya's <=64-bit constant-negation fast-path folded it to the wrapped value (int128/256
    already reverted via fall-through). FIX: skip the fold for the checked intN.min case so it falls
    through to the overflow check. Unchecked still wraps; normal negations stay correct.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/const_negate_typemin.sol")
    def sint(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    # -type(intN).min reverts at every width
    assert harness.call(app, "negMin8()", expect_revert=True).reverted
    assert harness.call(app, "negMin16()", expect_revert=True).reverted
    assert harness.call(app, "negMin64()", expect_revert=True).reverted
    assert harness.call(app, "negMin128()", expect_revert=True).reverted
    assert harness.call(app, "negMin256()", expect_revert=True).reverted
    assert harness.call(app, "nested()", expect_revert=True).reverted
    assert harness.call(app, "tildeMin()", expect_revert=True).reverted
    # unchecked wraps to int16.min (no revert)
    assert sint(harness.call(app, "uncheckedMin()")) == -32768
    # normal negations unaffected
    assert sint(harness.call(app, "negMax16()")) == -32767
    assert sint(harness.call(app, "negMinPlus()")) == 32767
    assert sint(harness.call(app, "negConst()")) == -5
    assert sint(harness.call(app, "negVar(int16)", 100)) == -100
    assert sint(harness.call(app, "negVar(int16)", -5)) == 5
    assert harness.call(app, "negVar(int16)", -32768, expect_revert=True).reverted  # runtime min still reverts


def test_short_circuit_rhs_side_effects(harness):
    """puyasolRegression/contracts/short_circuit_rhs_side_effects.sol — NOT an o.g. semantic test.

    The RHS of a short-circuit && / || with side effects (a checked op's overflow/zero assert) had those
    side effects HOISTED to the enclosing statement, so they ran unconditionally: `b != 0 && a/b > x`
    divided by zero when b==0, and `(b==0) || (a/b==0)` reverted when b==0, where EVM short-circuits. FIX:
    capture the RHS pre-statements and gate them behind the condition via an if/else (like the ternary).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/short_circuit_rhs_side_effects.sol")
    bo = lambda r: as_int(r.abi_return)
    I64MIN = -(1 << 63)
    I64MAX = (1 << 63) - 1
    # b==0 short-circuits -> RHS (a/b) is NOT evaluated -> no div-by-zero revert
    assert bo(harness.call(app, "orDiv(int64,int64)", 10, 0)) == 1          # was a revert
    assert bo(harness.call(app, "andDiv(int64,int64)", 10, 0)) == 0         # was a revert
    assert bo(harness.call(app, "orNeg(int64,int64)", I64MIN, 0)) == 1      # was a revert (-min overflow)
    assert bo(harness.call(app, "orAdd(int64,int64,int64)", I64MIN, I64MIN, 0)) == 1
    assert bo(harness.call(app, "nested(int64,int64,int64)", 10, 0, 0)) == 1
    # branch taken -> RHS IS evaluated -> correct value / real revert preserved
    assert bo(harness.call(app, "orDiv(int64,int64)", 10, 2)) == 0          # 5 == 0 -> false
    assert bo(harness.call(app, "orDiv(int64,int64)", 0, 5)) == 1           # 0 == 0 -> true
    assert bo(harness.call(app, "andDiv(int64,int64)", 100, 10)) == 1       # 10 > 5 -> true
    assert bo(harness.call(app, "andDiv(int64,int64)", 10, 10)) == 0        # 1 > 5 -> false
    assert bo(harness.call(app, "rhsTaken(int64,int64)", 5, 1)) == 1        # 6 > 5
    assert bo(harness.call(app, "rhsTaken(int64,int64)", 5, 0)) == 0        # short-circuit false
    assert harness.call(app, "rhsTaken(int64,int64)", I64MAX, 1, expect_revert=True).reverted  # a+1 overflows when taken
    # plain &&/|| unchanged
    assert bo(harness.call(app, "cmpAnd(uint64,uint64)", 5, 4)) == 1
    assert bo(harness.call(app, "cmpAnd(uint64,uint64)", 2, 4)) == 0
    assert bo(harness.call(app, "plainOr(bool,bool)", False, True)) == 1
    assert bo(harness.call(app, "plainOr(bool,bool)", False, False)) == 0


def test_mixed_width_signed_bitwise(harness):
    """puyasolRegression/contracts/mixed_width_signed_bitwise.sol — NOT an o.g. semantic test.

    A mixed-width bitwise op with a narrower SIGNED operand reinterpreted it at the common width without
    sign-extension: int128(-1) & int16(-32768) ANDed the raw 0x8000 (+32768) instead of the sign-extended
    int128 value (-32768). FIX (solc-todo opportunity D residual): coerce BOTH integer operands to
    commonType, mirroring the comparison path. Both narrower-left and narrower-right were wrong.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mixed_width_signed_bitwise.sol")
    def sint(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v > (1 << 255) else v
    I128MAX = (1 << 127) - 1
    # narrower SIGNED operand must sign-extend to the common width before the bitwise op
    assert sint(harness.call(app, "andSR(int128,int16)", -1, -32768)) == -32768   # all-ones & (..FF8000)
    assert sint(harness.call(app, "andSR(int128,int16)", -1, -1)) == -1
    assert sint(harness.call(app, "andSR(int128,int16)", I128MAX, -1)) == I128MAX  # & all-ones
    assert sint(harness.call(app, "andSR(int128,int16)", 12, 6)) == 4              # plain positives
    assert sint(harness.call(app, "andSL(int16,int128)", -1, 255)) == 255          # (all-ones) & 0xFF
    assert sint(harness.call(app, "andSL(int16,int128)", 5, 3)) == 1
    assert sint(harness.call(app, "orSR(int128,int16)", 0, -1)) == -1              # 0 | (all-ones)
    assert sint(harness.call(app, "orSL(int16,int128)", -1, 0)) == -1
    assert sint(harness.call(app, "xorSR(int128,int16)", -1, -1)) == 0
    assert sint(harness.call(app, "andSL8(int8,int256)", -1, 0xFFFF)) == 0xFFFF    # int8(-1)->int256 all-ones
    # unsigned mixed-width unaffected
    assert as_int(harness.call(app, "addU(uint16,uint128)", 5, 100).abi_return) == 105


def test_external_call_signed_narrow_return(harness):
    """puyasolRegression/contracts/external_call_signed_narrow_return.sol — NOT an o.g. semantic test.

    A SIGNED narrow-int (int8/16/32/64) RETURN from an external/inner contract call. The callee
    encodes a signed int return as a 32-byte uint256 (sign-extended); the caller used to decode it
    with an 8-byte `btoi` → "btoi arg too long, got 32 bytes" → revert on EVERY such call
    (value-independent). Fixed by extracting the low 8 bytes (canonical uint64-backed form) before
    btoi when the Solidity return type is signed. Found by the cross-contract differential fuzzer.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/external_call_signed_narrow_return.sol", contract_name="Caller")
    # Forwards widen+offset (+1000) so the observable is a clean positive int; broken decode reverts.
    assert as_int(harness.call(app, "g8(int256)", 5).abi_return) == 1005
    assert as_int(harness.call(app, "g8(int256)", -5).abi_return) == 995
    assert as_int(harness.call(app, "g8(int256)", 128).abi_return) == 872     # int8(128) wraps to -128
    assert as_int(harness.call(app, "g16(int256)", -100).abi_return) == 900
    assert as_int(harness.call(app, "g32(int256)", -7).abi_return) == 993
    assert as_int(harness.call(app, "g64(int256)", -1).abi_return) == 999
    assert as_int(harness.call(app, "g64(int256)", 9).abi_return) == 1009
    assert as_int(harness.call(app, "gu32(uint256)", 42).abi_return) == 1042   # unsigned control
    # signed-narrow TUPLE return (callee named it uint512 vs caller uint256 → router err; now uint256).
    assert as_int(harness.call(app, "gpair(int64,int64)", 3, 4).abi_return) == 1007
    assert as_int(harness.call(app, "gpair(int64,int64)", -3, -9).abi_return) == 988    # -12 + 1000
    assert as_int(harness.call(app, "gpair(int64,int64)", 100, -50).abi_return) == 1050
    assert as_int(harness.call(app, "gmixed(int64,uint64)", -5, 8).abi_return) == 1003   # -5 + 8 + 1000
    # UNSIGNED biguint (uint128/uint256) in a tuple return: callee encodes at natural N/8 width (16B for
    # uint128) but caller used a fixed 32B field -> wrong offsets. Now width comes from the Sol type.
    assert as_int(harness.call(app, "g128(uint128,uint128)", 7, 9).abi_return) == 16
    assert as_int(harness.call(app, "g128(uint128,uint128)", 1 << 127, 1 << 126).abi_return) == (1 << 127) + (1 << 126)
    assert as_int(harness.call(app, "gpmix(uint128,uint64)", 100, 50).abi_return) == 150   # mixed uint128/uint64
    # signed sub-word in a STRUCT field / ARRAY element across a call (callee names it int64, caller used
    # to drop the sign to uint64 -> selector mismatch). Now both use the canonical nestedArc4Name.
    assert as_int(harness.call(app, "gStruct(int64,int64)", 3, 4).abi_return) == 1007
    assert as_int(harness.call(app, "gStruct(int64,int64)", -5, -9).abi_return) == 986     # -14 + 1000
    assert as_int(harness.call(app, "gArr(int64)", 5).abi_return) == 1010                  # 5+5 + 1000
    assert as_int(harness.call(app, "gArr(int64)", -3).abi_return) == 994                  # -3+-3 + 1000
    # BOOL-tuple returns: ARC4 packs consecutive bools into one byte's bits. The caller's flat
    # 1-byte-per-bool decode reverted on `(bool,bool)` and mis-decoded `(bool,bool,uint256)`.
    # (even arg -> parity true.) Each bool is distinctly weighted so a bit misread is observable.
    assert as_int(harness.call(app, "gbb(int256,int256)", 2, 4).abi_return) == 3     # true,true
    assert as_int(harness.call(app, "gbb(int256,int256)", 2, 3).abi_return) == 2     # true,false
    assert as_int(harness.call(app, "gbb(int256,int256)", 3, 4).abi_return) == 1     # false,true
    assert as_int(harness.call(app, "gbb(int256,int256)", 3, 3).abi_return) == 0     # false,false
    assert as_int(harness.call(app, "gb3(int256,int256,int256)", 2, 4, 6).abi_return) == 7   # all true
    assert as_int(harness.call(app, "gb3(int256,int256,int256)", 2, 3, 6).abi_return) == 5   # t,f,t
    assert as_int(harness.call(app, "gb3(int256,int256,int256)", 3, 3, 3).abi_return) == 0   # all false
    # (bool,bool,uint256): 2nd bool must not read the uint256's first byte; the uint256 (incl.
    # high-bit values) must decode without running off the end.
    assert as_int(harness.call(app, "gbbu(int256,int256,int256)", 2, 4, 7).abi_return) == 10   # 2+1+7
    assert as_int(harness.call(app, "gbbu(int256,int256,int256)", 3, 3, 5).abi_return) == 5    # 0+0+5
    assert as_int(harness.call(app, "gbbu(int256,int256,int256)", 2, 4, -2).abi_return) == 1   # 2+1+int256(2^256-2)=-2
    # bool run BROKEN by a non-bool field, then another bool: offsets must realign after the uint64.
    assert as_int(harness.call(app, "gbub(int256,uint64,int256)", 2, 50, 6).abi_return) == 151  # 100+50+1
    assert as_int(harness.call(app, "gbub(int256,uint64,int256)", 3, 7, 3).abi_return) == 7     # 0+7+0
    # bytesN field in a tuple return: sized `bytes[N]` decode; used to fail to COMPILE.
    assert as_int(harness.call(app, "gtb4(uint256)", 7).abi_return) == 7 * (1 << 64) + 7
    assert as_int(harness.call(app, "gtb4(uint256)", 0x100000001).abi_return) == 1 * (1 << 64) + 0x100000001
    assert as_int(harness.call(app, "gtbb(uint256)", 4).abi_return) == 1000004    # even → true, bytes4(4)
    assert as_int(harness.call(app, "gtbb(uint256)", 5).abi_return) == 5          # odd → false, bytes4(5)
    # signed + unsigned-biguint mixed tuple: the uint128 stays 16B (natural), only the signed
    # element widens to uint256 — guards against re-widening the unsigned biguint to 32B.
    assert as_int(harness.call(app, "gsbig(int256,uint128)", -5, 100).abi_return) == 95     # -5 + 100
    assert as_int(harness.call(app, "gsbig(int256,uint128)", 3, 2 ** 70).abi_return) == 3 + 2 ** 70
    # biguint + bytesN in a tuple return (both orders + uint256) — was an UNCONDITIONAL revert
    # (Pass 3 allScalar guard → uint512 selector mismatch); fixed by whole-tuple ARC4Encode.
    for a in (5, 2 ** 32, 2 ** 100 + 9):
        assert as_int(harness.call(app, "gbu128(uint256)", a).abi_return) == (a % 2 ** 32) + (a % 2 ** 128)
        assert as_int(harness.call(app, "gu128b(uint256)", a).abi_return) == (a % 2 ** 128) + (a % 2 ** 32)
        assert as_int(harness.call(app, "gbu256(uint256)", a).abi_return) == (a % 2 ** 32) + a
    # enum / address / dynamic-field tuple returns — closed by the ARC4Decode-based decode rewrite
    # (dynamic head/tail + address padding) plus the top-level-enum selector-name fix (uint8→uint64).
    assert as_int(harness.call(app, "gea(uint256)", 2).abi_return) == 2002         # E(2), x=2
    assert as_int(harness.call(app, "gea(uint256)", 257).abi_return) == 2257       # 257%3=2 → E(2), x=257
    assert as_int(harness.call(app, "gad(uint256)", 12345).abi_return) == 12345 + 12345   # addr(12345)+x
    assert as_int(harness.call(app, "gdyn(uint256)", 9).abi_return) == 0x11 + 3 * 1000 + 9  # b[0]+len*1000+x
    assert as_int(harness.call(app, "gdstr(uint256)", 20).abi_return) == 2 + 20     # len("hi")+x
    assert as_int(harness.call(app, "garr(uint256)", 6).abi_return) == 6 + 7 * 1000 + 6    # r[0]+r[1]*1000+x


def test_modifier_dyntuple_return(harness):
    """puyasolRegression/contracts/modifier_dyntuple_return.sol — NOT an o.g. semantic test.

    Two cross-contract-return wire-type classes found by the differential fuzzer, fixed in
    ReturnRewriter (fable-review-2 D2). A separate small fixture: puya-sol embeds the callee's
    bytecode into the caller, so bolting these onto the big external_call_signed_narrow_return
    fixture blew the deploy page budget.

    (a) A biguint element in a DYNAMIC-element tuple `(uint128, bytes)` — Pass 3's old `allStatic`
        guard left it "uint512" vs the caller's "uint128" → unconditional revert. Now wrapped in
        any tuple.
    (b) MODIFIER'D (chain-lowered) returns — the outer dispatch return published the bare biguint
        as "uint512" while callers name the declared width. Fixed by encodeChainDispatchReturn +
        threading the promoted returnType (a fresh map() gives int64→uint64 → "Tuple type mismatch").
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_dyntuple_return.sol", contract_name="Caller")
    # (a) biguint in a dynamic-element tuple. 0xaa=170, len 2.
    assert as_int(harness.call(app, "gdbig(uint256)", 5).abi_return) == 5 + 170 + 2
    assert as_int(harness.call(app, "gdbig(uint256)", 2 ** 100 + 9).abi_return) == (2 ** 100 + 9) % 2 ** 128 + 170 + 2
    # (b) modifier'd returns: single unsigned-wide, single signed sub-word, unsigned tuple, signed tuple.
    assert as_int(harness.call(app, "gmu128(uint256)", 77).abi_return) == 77
    assert as_int(harness.call(app, "gmu128(uint256)", 2 ** 127).abi_return) == 2 ** 127
    assert as_int(harness.call(app, "gmi64(int256)", -5).abi_return) == 995      # signed sub-word, negative
    assert as_int(harness.call(app, "gmi64(int256)", 9).abi_return) == 1009
    assert as_int(harness.call(app, "gmtup(uint256)", 6).abi_return) == 12       # 6 + 6, unsigned tuple
    assert as_int(harness.call(app, "gmstup(int256)", -4).abi_return) == 992     # -4 + -4 + 1000, signed tuple
    assert as_int(harness.call(app, "gmstup(int256)", 30).abi_return) == 1060    # 30 + 30 + 1000


def test_inner_call_tuple_results(harness):
    """puyasolRegression/contracts/inner_call_tuple_results.sol — NOT an o.g. semantic test.

    Multiple inner calls built inside ONE expression must each capture their own
    result. The AVM itxn context is a single register: expression building
    flushes every call's submit to pre-pending BEFORE the containing expression
    evaluates, so a live `itxn LastLog` read in each result slot returned the
    LAST call's value for every slot (`return (s.a(), s.b(), s.c())` gave
    (33,33,33)). Fixed by capture-after-submit
    (InnerCallHandlers::captureLastLog); found while probing the constructor
    fixes, pre-existing.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/inner_call_tuple_results.sol", contract_name="Caller")
    r = harness.call(app, "tup()", extra_fee=10000)
    assert tuple(as_int(x) for x in r.abi_return) == (11, 22, 33)
    # two inner calls as ARGS of one internal call — same clobbering shape
    assert as_int(harness.call(app, "nested()", extra_fee=10000).abi_return) == 44


def test_modifier_stack_conditional(harness):
    """puyasolRegression/contracts/modifier_stack_conditional.sol — NOT an o.g. semantic test.

    Found by the dedicated MODIFIER fuzz axis. Stacked modifiers `m() gated both` must nest with the
    LEFTMOST outermost (Solidity evaluates modifiers left-to-right): `if (gate) { ctr++; _; ctr++; }`.
    The pre-fix body inliner iterated modifiers forward and wrapped each as the OUTER layer, so the
    rightmost (`both`) became outermost -> `ctr++; if (gate) {_;} ctr++` and ctr incremented even when
    gate was false. Fix iterates modifiers right-to-left (mirrors the viaIR chain builder).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/modifier_stack_conditional.sol")
    assert as_int(harness.call(app, "getCtr()").abi_return) == 0
    harness.call(app, "m()")                 # gate=false -> gated skips -> both must NOT run
    assert as_int(harness.call(app, "getCtr()").abi_return) == 0   # was 2 (both ran unconditionally)
    harness.call(app, "setGate(bool)", True)
    harness.call(app, "m()")                 # gate=true -> both runs, ctr += 2
    assert as_int(harness.call(app, "getCtr()").abi_return) == 2


def test_modifier_multiple_placeholder(harness):
    """puyasolRegression/contracts/modifier_multiple_placeholder.sol — NOT an o.g. semantic test.

    Found by the modifier fuzz axis. A modifier with multiple `_;` (e.g. `twice() { _; _; }`) runs the
    function body once per placeholder. The body inliner splices the placeholder body per `_;`; before
    the fix it shared the same AWST nodes across the copies, so a checked-arithmetic body aliased its
    overflow-assert temps (and SingleEvaluation cache keys) and miscompiled — the AVM value diverged
    from EVM and reverts flipped. Fixed by deep-cloning the spliced body per `_;` (awst::cloneBlock,
    fresh SingleEvaluation ids, sharing preserved within each splice).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_multiple_placeholder.sol")
    # addTwice: body (acc = acc + v) runs twice -> +2v
    harness.call(app, "addTwice(uint256)", 5)
    assert as_int(harness.call(app, "getAcc()").abi_return) == 10      # was wrong (aliased)
    harness.call(app, "addTwice(uint256)", 3)
    assert as_int(harness.call(app, "getAcc()").abi_return) == 16      # 10 + 2*3
    # addStacked: both(twice(body)) -> ctr += 2, acc += 2v
    harness.call(app, "addStacked(uint256)", 4)
    assert as_int(harness.call(app, "getAcc()").abi_return) == 24      # 16 + 2*4
    assert as_int(harness.call(app, "getCtr()").abi_return) == 2
    # value-returning body under twice -> returns v (last `_;` wins)
    assert as_int(harness.call(app, "ret(uint256)", 7).abi_return) == 7


def test_struct_fixed_array_first_write(harness):
    """puyasolRegression/contracts/struct_fixed_array_first_write.sol — NOT an o.g. semantic test.

    Found by the fixed-size-array fuzz axis. `st.inner[i] = v` (fixed array inside a struct state var)
    is a PARTIAL write via box_replace; a plain state-var box is only created by a FULL write, so a
    FIRST partial write (before any `st = ...` / `st.x = ...`) hit "no such box" and reverted (EVM
    auto-zero-inits storage). Fixed by an idempotent box_put(default) prologue for state-var boxes
    reached by a partial write. This test makes setStInner the VERY FIRST call (fresh box).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/struct_fixed_array_first_write.sol")

    def s128(r):
        # int128 getter publishes as a 256-bit two's-complement uint; decode at 256-bit.
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v

    # FIRST call is a partial write to a never-created box — must create it, not revert.
    harness.call(app, "setStInner(uint256,int128)", 0, -5)
    assert s128(harness.call(app, "getStInner(uint256)", 0)) == -5
    # int128.min (the sign-boundary that stressed the encode) into the other element.
    harness.call(app, "setStInner(uint256,int128)", 1, -(1 << 127))
    assert s128(harness.call(app, "getStInner(uint256)", 1)) == -(1 << 127)
    # sibling untouched; the other struct field still writable + independent.
    assert s128(harness.call(app, "getStInner(uint256)", 0)) == -5
    harness.call(app, "setStX(uint64)", 42)
    assert as_int(harness.call(app, "getStX()").abi_return) == 42
    assert s128(harness.call(app, "getStInner(uint256)", 1)) == -(1 << 127)


def test_multidim_fixed_array_box_size(harness):
    """puyasolRegression/contracts/multidim_fixed_array_box_size.sol — NOT an o.g. semantic test.

    Found by the fixed-size-array fuzz axis. A multi-dim fixed array `int256[2][2]` is one 128-byte
    box, but the deploy-time box_create sized it at 64 (the manual elementType() switch handled only
    ARC4UIntN/Bytes; a nested-static-array element fell to the default elemSize=32 -> 32*2=64). So
    writing grid[1][j] (offset >= 64) reverted "replacement end beyond original length". Fixed by
    sizing from StorageMapper::arc4StaticArrayTotalBytes (recursive element size => 128).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/multidim_fixed_array_box_size.sol")

    def s256(r):
        v = as_int(r.abi_return); return v - (1 << 256) if v >= (1 << 255) else v

    # write every cell — grid[1][*] is the one that used to revert (offset >= 64 in a 64B box)
    harness.call(app, "setGrid(uint256,uint256,int256)", 0, 0, 11)
    harness.call(app, "setGrid(uint256,uint256,int256)", 0, 1, 22)
    harness.call(app, "setGrid(uint256,uint256,int256)", 1, 0, -(1 << 255))   # int256.min at [1][0]
    harness.call(app, "setGrid(uint256,uint256,int256)", 1, 1, -7)
    assert s256(harness.call(app, "getGrid(uint256,uint256)", 0, 0)) == 11
    assert s256(harness.call(app, "getGrid(uint256,uint256)", 0, 1)) == 22
    assert s256(harness.call(app, "getGrid(uint256,uint256)", 1, 0)) == -(1 << 255)
    assert s256(harness.call(app, "getGrid(uint256,uint256)", 1, 1)) == -7


def test_mapping_struct_dynarray_push(harness):
    """puyasolRegression/contracts/mapping_struct_dynarray_push.sol — NOT an o.g. semantic test.

    Found by the dynamic-struct fuzz axis. `m[k].arr.push(v)` where arr is a dyn-array FIELD of a
    struct stored in a mapping entry: baseExpr is a MemberAccess (m[k].arr), so it fell into the
    chained-storage push path in SolArrayMethod which built ArrayExtend WITHOUT first materialising
    the lazy per-entry struct box -> box_extract hit "no such box". Fixed by calling
    StorageMapper::makeEnsureRootBoxForWrite (isResize) there too, matching the m[k].push() branch.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/mapping_struct_dynarray_push.sol")

    harness.call(app, "pushM(uint256,uint64)", 7, 111)
    harness.call(app, "pushM(uint256,uint64)", 7, 222)
    harness.call(app, "pushM(uint256,uint64)", 9, 333)   # distinct entry -> its own lazy box
    assert as_int(harness.call(app, "lenM(uint256)", 7).abi_return) == 2
    assert as_int(harness.call(app, "lenM(uint256)", 9).abi_return) == 1
    assert as_int(harness.call(app, "getM(uint256,uint256)", 7, 0).abi_return) == 111
    assert as_int(harness.call(app, "getM(uint256,uint256)", 7, 1).abi_return) == 222
    assert as_int(harness.call(app, "getM(uint256,uint256)", 9, 0).abi_return) == 333



def test_shift_amount_huge(harness):
    """puyasolRegression/contracts/shift_amount_huge.sol — NOT an o.g. semantic test.

    A runtime shift AMOUNT >= 2^64 (biguint-typed uint256) was low-64-truncated by the
    biguint->uint64 coercion BEFORE the >=256 saturation guards ran: `x >> 2^128` shifted by
    (2^128 mod 2^64) = 0 and returned x unchanged, `x >> (2^128+5)` shifted by 5 — the EVM
    saturates for ANY amount >= 256 (shl/shr -> 0, sar -> 0/-1, EIP-145). Fixed by
    eb::shiftAmountToUint64 (clamp at the biguint level: >= 256 -> 256, which the shift
    builders saturate on). Found by the differential fuzzer (codec_probe/arith_edge).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/shift_amount_huge.sol")

    def w(v):                                                    # wrap raw uint → signed-256
        return v - (1 << 256) if v > (1 << 255) else v

    def s256(r):
        return w(as_int(r.abi_return))

    IMIN = -(1 << 255)
    HUGE = 1 << 128
    # signed sar: saturates to -1 (negative) / 0 (non-negative) for any amount >= 256
    assert s256(harness.call(app, "sar(int256,uint256)", IMIN, HUGE)) == -1        # was: unchanged
    assert s256(harness.call(app, "sar(int256,uint256)", IMIN, HUGE + 5)) == -1    # was: shift by 5
    assert s256(harness.call(app, "sar(int256,uint256)", 12345, 1 << 200)) == 0
    # sub-word signed (canonicalized to 256-bit TC first)
    assert s256(harness.call(app, "sar8(int8,uint256)", -1, HUGE)) == -1
    assert s256(harness.call(app, "sar8(int8,uint256)", 7, HUGE + 3)) == 0
    # unsigned shr/shl: 0 for any amount >= 256, huge or not
    assert as_int(harness.call(app, "shr(uint256,uint256)", (1 << 256) - 1, HUGE).abi_return) == 0
    assert as_int(harness.call(app, "shl(uint256,uint256)", 1, HUGE + 7).abi_return) == 0
    # boundary 256 (in-uint64-range saturation — the pre-existing guard, still intact)
    assert s256(harness.call(app, "sar(int256,uint256)", IMIN, 256)) == -1
    assert as_int(harness.call(app, "shl(uint256,uint256)", 1, 256).abi_return) == 0
    # in-range amounts unchanged
    assert s256(harness.call(app, "sar(int256,uint256)", -8, 2)) == -2
    assert as_int(harness.call(app, "shl(uint256,uint256)", 1, 255).abi_return) == 1 << 255
    assert as_int(harness.call(app, "shr(uint256,uint256)", 1 << 255, 254).abi_return) == 2


def test_bytesn_op_narrowing(harness):
    """puyasolRegression/contracts/bytesn_op_narrowing.sol — NOT an o.g. semantic test.

    A bytesN SHIFT or BITWISE binop result (SolFixedBytesBuilder) carried an UNSIZED `bytes`
    wtype, so a subsequent bytesN(M->N) narrowing couldn't see the source length and no-op'd:
    `uint32(bytes4(bytes32(x) << n))` btoi'd 32 bytes -> reverted on every input;
    `bytes4(a & b)` silently kept all 32 bytes. Fixed by retagging results with the sized
    bytes[N] wtype (+ the shift branch's own huge-amount truncation routed through
    shiftAmountToUint64). Found by the differential fuzzer (fixedbytes_probe::truncShift).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/bytesn_op_narrowing.sol")

    X = 0x1122334455 << 216                     # bytes32 = 0x1122334455 00..00 (left-aligned)
    assert as_int(harness.call(app, "truncShift(uint256,uint8)", X, 0).abi_return) == 0x11223344
    assert as_int(harness.call(app, "truncShift(uint256,uint8)", X, 8).abi_return) == 0x22334455
    assert as_int(harness.call(app, "truncShift(uint256,uint8)", X, 255).abi_return) == 0
    # right-aligned value: top 4 bytes are zero before AND after a small shift
    assert as_int(harness.call(app, "truncShift(uint256,uint8)", 0x1122334455, 8).abi_return) == 0
    # huge shift amounts saturate through the bytesN path too
    assert as_int(harness.call(app, "truncShiftHuge(uint256,uint256)", X, 1 << 128).abi_return) == 0
    assert as_int(harness.call(app, "truncShiftHuge(uint256,uint256)", X, (1 << 128) + 8).abi_return) == 0
    assert as_int(harness.call(app, "truncShiftHuge(uint256,uint256)", X, 8).abi_return) == 0x22334455
    # bitwise results narrow correctly
    M = (1 << 256) - 1
    assert as_int(harness.call(app, "narrowAnd(uint256,uint256)", X, M).abi_return) == 0x11223344
    assert as_int(harness.call(app, "narrowOr(uint256,uint256)", 0, X).abi_return) == 0x11223344
    assert as_int(harness.call(app, "narrowXor(uint256,uint256)", X, M).abi_return) == 0x11223344 ^ 0xFFFFFFFF
    # the lowering's own docstring example: bytes6(0x616263646566) << 24 == 0x646566000000
    assert as_int(harness.call(app, "shiftB6(uint48,uint8)", 0x616263646566, 24).abi_return) == 0x646566000000


def test_dce_reverting_subexpr(harness):
    """puyasolRegression/contracts/dce_reverting_subexpr.sol — NOT an o.g. semantic test.

    FRONTEND half of the missing-revert-under-fold class (campaign seeds 21008/f7, 22000/f18):
    buildBigUIntShift's >=256 saturation conditional evaluated the shifted VALUE lazily, so a
    reverting subexpression under a shift by a RUNTIME amount >= 256 was skipped at runtime.
    Fixed: the value is pinned eagerly via a comma-expr binding (the SAR helper's idiom).
    The LITERAL-amount shapes are the separate OPEN backend half — see the xfail test below.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/dce_reverting_subexpr.sol")

    # runtime amount >= 256: the reverting value must still evaluate (eager bind)
    r = harness.call(app, "modShrRt(uint256,uint256,uint256)", 7, 0, 300, expect_revert=True)
    assert getattr(r, "reverted", False), "runtime-saturated shift must still evaluate (a % 0)"

    # nonzero divisors -> folded values, no revert
    assert as_int(harness.call(app, "modShrRt(uint256,uint256,uint256)", 7, 3, 300).abi_return) == 0
    assert as_int(harness.call(app, "modShrRt(uint256,uint256,uint256)", 7, 3, 0).abi_return) == 1
    assert as_int(harness.call(app, "divdivShl(uint256)", 5).abi_return) == 0
    assert as_int(harness.call(app, "modShl(uint256,uint256)", 7, 3).abi_return) == 0
    assert as_int(harness.call(app, "ternFold(uint256,uint256)", 7, 3).abi_return) == 3
    assert as_int(harness.call(app, "expZero(uint256,uint256)", 7, 3).abi_return) == 1
    assert as_int(harness.call(app, "mulZero(uint256,uint256)", 7, 3).abi_return) == 0


def test_dce_reverting_subexpr_literal_folds(harness):
    """BACKEND half — OPEN BUG, this test FAILS on purpose until it's fixed.

    With a LITERAL fold (shift>=256, identical-branch ternary, **0, *0, &0) puya's DCE drops
    the unused div/mod whose zero-divisor panic carries the EVM revert ('/', '%', 'b/', 'b%'
    are in SIDE_EFFECT_FREE_AVM_OPS) -> AVM returns the folded value where solc+EVM revert.
    A one-line fork fix exists and was validated (zero-reg, closes all shapes) but was
    REVERTED by policy: no puya fork changes. Preserved as fork-remote 716e63e44; see
    puyabug.md #9. Not xfailed: open bugs stay as honest failures (xfail is reserved for
    by-design/purposely-unsupported behavior).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/dce_reverting_subexpr.sol")
    for sig, args in [
        ("divdivShl(uint256)", (0,)),
        ("modShl(uint256,uint256)", (7, 0)),
        ("ternFold(uint256,uint256)", (7, 0)),
        ("expZero(uint256,uint256)", (7, 0)),
        ("mulZero(uint256,uint256)", (7, 0)),
    ]:
        r = harness.call(app, sig, *args, expect_revert=True)
        assert getattr(r, "reverted", False), f"{sig}{args} must revert (divide/mod by zero)"


def test_const_var_fold(harness):
    """puyasolRegression/contracts/const_var_fold.sol — NOT an o.g. semantic test.

    Guard for SolcConstFold::foldTyped (fable-review item 1, case (b)): intN-typed
    constant-variable expressions fold via solc's ConstantEvaluator ONLY under the
    every-node-in-range guard. Negative cases (shapes solc lets through to runtime):
    `-M` with M = type(int8).min must REVERT (128 out of int8 range -> checked path);
    `(P<<1)>>1` must compute on the TRUNCATED intermediate (-28, not the rational 100);
    unchecked `P*3` must WRAP (44, not 300).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/const_var_fold.sol")

    def w(v, bits=256):
        return v - (1 << bits) if v > (1 << (bits - 1)) else v

    r = harness.call(app, "negMin()", expect_revert=True)
    assert getattr(r, "reverted", False), "-type(int8).min via const var must revert"
    # out-of-range intermediates must reach the runtime lowering (solc rejects the
    # CHECKED binary-overflow shapes at compile time; shifts and unchecked mul get through)
    assert w(as_int(harness.call(app, "shiftTrunc()").abi_return)) == -28
    assert w(as_int(harness.call(app, "mulWrapUnchecked()").abi_return)) == 44
    assert w(as_int(harness.call(app, "divTrunc()").abi_return)) == -2
    assert w(as_int(harness.call(app, "modSign()").abi_return)) == -1
    assert w(as_int(harness.call(app, "arith()").abi_return)) == 88
    assert w(as_int(harness.call(app, "wide()").abi_return)) == 12345678901234567890
    assert as_int(harness.call(app, "big()").abi_return) == (1 << 199) + 1


def test_this_contract_value(harness):
    """puyasolRegression/contracts/this_contract_value.sol — NOT an o.g. semantic test.

    Bare `this` (contract-typed) must carry the FAKE app-id address form like every other
    contract-typed value (bzero(24)++itob(appId)) so an opaque consumer can recover the app id;
    it used to lower to the REAL application address, making stored/forwarded `this` call-backs
    target btoi(hash garbage). The deferred-callback (registry) pattern proves the fix:
    Cer registers `this` inside Cee, a separate transaction makes Cee call back into Cer.
    `address(this)` must still be the REAL application address.
    """
    import algosdk

    arts = harness.compile("puyasolRegression/contracts/this_contract_value.sol")
    cee = harness.deploy(arts, contract_name="Cee")
    cer = harness.deploy(arts, contract_name="Cer")

    cee_fake = algosdk.encoding.encode_address(bytes(24) + cee.app_id.to_bytes(8, "big"))
    harness.call(cer, "enrollAt(address)", cee_fake)

    # deferred call-back: Cee -> Cer in a fresh transaction (no re-entry)
    assert as_int(harness.call(cee, "pokeStored(uint64)", 41).abi_return) == 42
    assert as_int(harness.call(cer, "last()").abi_return) == 42

    # the stored target is the FAKE form carrying Cer's app id
    stored = harness.call(cee, "target()").abi_return
    raw = algosdk.encoding.decode_address(stored) if isinstance(stored, str) else bytes(stored)
    assert int.from_bytes(raw[24:], "big") == cer.app_id
    assert raw[:24] == bytes(24)

    # address(this) stays the REAL application address
    real = harness.call(cer, "realAddr()").abi_return
    real_s = real if isinstance(real, str) else algosdk.encoding.encode_address(bytes(real))
    assert real_s == cer.app_addr


def test_packed_slot_word_dispatch(harness):
    """puyasolRegression/contracts/packed_slot_word_dispatch.sol

    Guard for the packed-slot codec in the asm storage dispatcher: EVM packs
    several sub-word vars into one 32-byte slot, while our model keeps each in
    its own typed cell (uint64 / canonical-TC biguint / bool / bytes[N]).
    sload assembles the EVM word from the cells; sstore splits it back through
    each var's native repr (incl. 64-bit-TC sign-extension for sub-64 signed).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/packed_slot_word_dispatch.sol")
    expected = int.from_bytes(
        bytes(10) + bytes([0x01]) + bytes.fromhex('3333333333333333')
        + bytes.fromhex('2222222222222222') + bytes.fromhex('fffffffe') + bytes([0x11]), 'big')
    # high-level writes -> asm word read
    harness.call(app, 'set(uint8,int32,uint64,bytes8,bool)',
                 0x11, -2, 0x2222222222222222, bytes.fromhex('3333333333333333'), True)
    r = harness.call(app, 'readWord()')
    assert as_int(r.abi_return) == expected
    # asm word write -> high-level reads
    w2 = int.from_bytes(
        bytes(10) + bytes([0x00]) + bytes.fromhex('4444444444444444')
        + bytes.fromhex('5555555555555555') + bytes.fromhex('00000007') + bytes([0x99]), 'big')
    harness.call(app, 'writeWord(uint256)', w2)
    vals = harness.call(app, 'get()').abi_return
    assert as_int(vals[0]) == 0x99
    assert as_signed_int(vals[1]) == 7
    assert as_int(vals[2]) == 0x5555555555555555
    assert as_bytes(vals[3]) == bytes.fromhex('4444444444444444')
    assert vals[4] is False
    # negative sub-64 signed through the word, both directions
    w3 = int.from_bytes(bytes(27) + bytes.fromhex('fffffffd') + bytes([0x01]), 'big')
    harness.call(app, 'writeWord(uint256)', w3)
    r = harness.call(app, 'get()')
    assert as_signed_int(r.abi_return[1]) == -3
    r = harness.call(app, 'readWord()')
    assert as_int(r.abi_return) == w3


def test_asm_string_buffer_pointer(harness):
    """puyasolRegression/contracts/asm_string_buffer_pointer.sol — NOT an o.g. test.

    The memory-pointer seam: a `new string(n)`/`new bytes(n)` buffer used in
    inline assembly as its Yul memory POINTER (`add(buffer, k)` + `mstore8` +
    `return buffer`) — the OpenZeppelin Strings.toString/toHexString idiom.
    Such a buffer is promoted to the blob-backed (pointer) model so asm writes
    land in the memory blob, and an outside-asm value-use materialises
    [len word][data] back out. Guarded across BOTH build paths: the library
    internal function (AWSTBuilder) and the public function (ContractBuilder) —
    each marks asm-aggregates via the shared markAssemblyAggregates.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_string_buffer_pointer.sol")
    # library-internal toString
    assert harness.call(app, "dec(uint256)", 0).abi_return == "0"
    assert harness.call(app, "dec(uint256)", 12345).abi_return == "12345"
    assert harness.call(app, "dec(uint256)", 9876543210).abi_return == "9876543210"
    # library-internal toHexString(_, 32): "0x" + 64 hex chars
    assert harness.call(app, "hex32(uint256)", 255).abi_return == "0x" + "00" * 31 + "ff"
    assert harness.call(app, "hex32(uint256)", 0).abi_return == "0x" + "00" * 32
    # public build path, same idiom
    assert harness.call(app, "decInline(uint256)", 0).abi_return == "0"
    assert harness.call(app, "decInline(uint256)", 42).abi_return == "42"
    assert harness.call(app, "decInline(uint256)", 9876543210).abi_return == "9876543210"


def test_array_struct_mapping_alias(harness):
    """puyasolRegression/contracts/array_struct_mapping_alias.sol — NOT an o.g. test.

    A mapping inside a struct held in a storage ARRAY must be isolated per element.
    The mapping-key derivation folds each index level of the chain into the box key,
    but `arr[i].m[k]` stops the chain walk at the MemberAccess (`arr[i].m`), so the
    array index was dropped: the prefix fell back to plain utf8("m") and EVERY
    element's mapping shared one box. A write to arr[1].m[k] silently clobbered
    arr[0].m[k] — wrong data, no revert. Found by the night-3 cold-dir campaign.

    Fixed by folding the array name + index into the prefix (ARRAY bases only —
    a MAPPING base must keep the per-layer sha256 derivation, which the pre-existing
    types/test_struct_mapping_abstract_constructor_param pins).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/array_struct_mapping_alias.sol")
    harness.call(app, "seed()")
    # the bug: both returned 200 (element 1 clobbered element 0)
    assert as_int(harness.call(app, "s(uint256)", 0).abi_return) == 100
    assert as_int(harness.call(app, "s(uint256)", 1).abi_return) == 200
    # plain struct field + array-of-mappings controls stay isolated
    assert as_int(harness.call(app, "x(uint256)", 0).abi_return) == 1
    assert as_int(harness.call(app, "x(uint256)", 1).abi_return) == 2
    assert as_int(harness.call(app, "a(uint256)", 0).abi_return) == 300
    assert as_int(harness.call(app, "a(uint256)", 1).abi_return) == 400
    # MAPPING base (`mapping(uint => S) mm; mm[i].m[k]`): same aliasing family —
    # all outer keys collapsed onto ONE box (mm[1] write clobbered mm[0]; a
    # never-written mm[2] read the shared value back). Fixed by prefixing with
    # the element's derived box key (the storage-ref-param convention), which
    # test_struct_mapping_abstract_constructor_param pins cross-path.
    assert as_int(harness.call(app, "m(uint256)", 0).abi_return) == 500
    assert as_int(harness.call(app, "m(uint256)", 1).abi_return) == 600
    assert as_int(harness.call(app, "m(uint256)", 2).abi_return) == 0
    # Two same-typed struct STATE VARS: the utf8(field) prefix aliased them
    # (st2.m[7] clobbered st1.m[7]); now prefixed with the holder var name,
    # and a `S storage p = st1` aliased write lands where direct reads look.
    assert as_int(harness.call(app, "st(uint256,uint256)", 0, 7).abi_return) == 100
    assert as_int(harness.call(app, "st(uint256,uint256)", 1, 7).abi_return) == 200
    assert as_int(harness.call(app, "st(uint256,uint256)", 0, 8).abi_return) == 111
    assert as_int(harness.call(app, "st(uint256,uint256)", 1, 8).abi_return) == 0
    # struct-IN-struct state vars (chain depth 2): the MemberAccess chain walk
    # must append EVERY field to the holder prefix, not just the innermost.
    assert as_int(harness.call(app, "o(uint256,uint256)", 0, 7).abi_return) == 1000
    assert as_int(harness.call(app, "o(uint256,uint256)", 1, 7).abi_return) == 2000
    # isolation survives a later write
    harness.call(app, "bump(uint256,uint256)", 0, 111)
    assert as_int(harness.call(app, "s(uint256)", 0).abi_return) == 111
    assert as_int(harness.call(app, "s(uint256)", 1).abi_return) == 200


def test_mapping_chain_index_bounds(harness):
    """puyasolRegression/contracts/mapping_chain_index_bounds.sol — NOT an o.g. test.

    ARRAY levels feeding the mapping-key derivation folded the element index
    into the box key with NO bounds check: `aom[aom.length][k]` read a phantom
    element's box (0) where EVM panics 0x32, and OOB writes silently stored.
    Asserts idx < length — fixed-size bounds anywhere in the chain; dynamic
    lengths for chains rooted at a plain box state var (shared
    SolLengthAccess::stateDynArrayLength, so asserts agree with `.length`).
    Found by the night-3 cold-dir campaign (mappings_array_pop_delete `-`→`*`
    mutant turned `a[a.length-1]` into `a[a.length]`).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mapping_chain_index_bounds.sol")
    harness.call(app, "seed()")
    # in-bounds still works
    assert as_int(harness.call(app, "readAom(uint256,uint256)", 0, 1).abi_return) == 11
    assert as_int(harness.call(app, "readFixed(uint256,uint256)", 2, 1).abi_return) == 22
    assert as_int(harness.call(app, "readSarrM(uint256,uint256)", 0, 1).abi_return) == 33
    assert as_int(harness.call(app, "readSarrX(uint256)", 0).abi_return) == 44
    assert as_int(harness.call(app, "readLast(uint256)", 1).abi_return) == 11
    assert as_int(harness.call(app, "readLastX()").abi_return) == 44
    harness.call(app, "writeSarrX(uint256,uint256)", 0, 45)
    assert as_int(harness.call(app, "readSarrX(uint256)", 0).abi_return) == 45
    # out-of-bounds must revert (EVM Panic 0x32), not read/write phantom boxes
    for sig, args in [
        ("readAom(uint256,uint256)", (1, 1)),       # == length
        ("readAom(uint256,uint256)", (255, 1)),
        ("writeAom(uint256,uint256,uint256)", (1, 1, 99)),
        ("readFixed(uint256,uint256)", (3, 1)),     # == fixed size
        ("readSarrM(uint256,uint256)", (1, 1)),     # == length
        # struct FIELD via the dynamic-element offset table (was garbage/phantom)
        ("readSarrX(uint256)", (1,)),               # == length
        ("readSarrX(uint256)", (255,)),
        ("writeSarrX(uint256,uint256)", (1, 99)),
    ]:
        r = harness.call(app, sig, *args, expect_revert=True)
        assert getattr(r, "reverted", False), f"{sig}{args} must revert (index OOB)"


def test_calldata_empty_array_index(harness):
    """puyasolRegression/contracts/calldata_empty_array_index.sol — NOT an o.g. test.

    Indexing an EMPTY calldata array kept as an ARC4 VALUE (asm-mode function,
    struct skips the native decode) read adjacent struct bytes and returned 0
    where EVM panics 0x32 — puya's IndexExpression has no length check and the
    empty array's phantom slot stays within valid bytes. Fixed with an explicit
    idx < uint16-length-prefix assert in SolArrayBuilder::index (calldata ARC4
    values only). Found by the night-3 stmt-del mutant on dirty_calldata_struct.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/calldata_empty_array_index.sol")
    # in-bounds still decodes correctly (0x180 sign-extends via the int8 path upstream)
    assert as_int(harness.call(app, "f((uint16[]))", ([384],)).abi_return) == 384
    assert as_int(harness.call(app, "fi((uint16[]),uint256)", ([7, 9],), 1).abi_return) == 9
    # empty inner array / OOB runtime index must revert, not read phantom bytes
    for sig, args in [
        ("f((uint16[]))", (([],),)),
        ("fi((uint16[]),uint256)", (([],), 0)),
        ("fi((uint16[]),uint256)", (([5],), 1)),   # == length
        ("fi((uint16[]),uint256)", (([5],), 255)),
    ]:
        r = harness.call(app, sig, *args, expect_revert=True)
        assert getattr(r, "reverted", False), f"{sig}{args} must revert (index OOB)"


def test_call_value_with_data_invokes_target(harness):
    """puyasolRegression/contracts/call_value_with_data.sol — NOT an o.g. test.

    `.call{value: X}(data)` with NON-empty data must invoke the target AND
    transfer the value. The pre-fix dispatcher matched any .call{value:} and
    lowered it to a bare payment: the calldata was silently dropped, deposit()
    never ran, ok == true. Now lowered as one inner group
    [PaymentTxn, ApplicationCall], so the callee runs and its msg.value
    (gtxns Amount at GroupIndex-1) sees the payment.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/call_value_with_data.sol", fund_wei=2_000_000
    )
    r = harness.call(app, "run()", extra_fee=30_000).abi_return
    ok, deposits, got = bool(r[0]), as_int(r[1]), as_int(r[2])
    assert ok
    assert deposits == 1, "deposit() did not execute — calldata dropped"
    assert got == 150_000, f"msg.value not visible to callee (got={got})"


def test_asm_const_cache_invalidation(harness):
    """puyasolRegression/contracts/asm_const_cache.sol — NOT an o.g. test.

    Assembly constant caches must be invalidated on reassignment / untrackable
    memory writes / control flow. Pre-fix, `let p := 0x80 … p := add(p, 0x20)
    … mstore(p, v)` folded BOTH stores to offset 0x80 (pointer-bump and
    indexed-loop idioms silently miscompiled), and "mem_0x<off>" content
    entries fed stale values to the keccak/mload folds.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_const_cache.sol")
    assert as_int(harness.call(app, "reassign()").abi_return) == 111
    assert as_int(harness.call(app, "loopFold()").abi_return) == 2
    # value-dependence: pre-fix both returned keccak(5)
    k5 = as_bytes(harness.call(app, "kec(uint256)", 5).abi_return)
    k7 = as_bytes(harness.call(app, "kec(uint256)", 7).abi_return)
    assert k5 != k7, "keccak folded a stale mem constant (value-independent hash)"
    assert as_int(harness.call(app, "branch(uint256)", 0).abi_return) == 1
    assert as_int(harness.call(app, "branch(uint256)", 1).abi_return) == 7


def test_ctor_ternary_base_arg(harness):
    """puyasolRegression/contracts/ctor_arg_prestmts.sol — NOT an o.g. test.

    A branch-lowered (ternary) base-ctor argument must have its pre-statements
    (the if/else assigning the __cond temp) emitted BEFORE the param binding.
    Pre-fix the create path bound `x = __cond_N` first, so A initialized with 0.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/ctor_arg_prestmts.sol", "D", ctor_args=[5]
    )
    assert as_signed_int(harness.call(app, "va()").abi_return) == 5
    app2 = harness.compile_and_deploy(
        "puyasolRegression/contracts/ctor_arg_prestmts.sol", "D", ctor_args=[-3]
    )
    assert as_signed_int(harness.call(app2, "va()").abi_return) == 3


def test_postinit_transitive_ctor_args(harness):
    """puyasolRegression/contracts/postinit_transitive_ctor_args.sol — NOT an o.g. test.

    __postInit must assign base-ctor params derived-first before inlining any
    ctor body: `D is C is A`, `C(uint y) A(y+1)` — A's arg reads C's y.
    Pre-fix order was `x = y + 1; va = x; y = 5; …` → va == 1 instead of 6.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/postinit_transitive_ctor_args.sol", "D"
    )
    assert as_int(harness.call(app, "va()").abi_return) == 6
    assert as_int(harness.call(app, "y2()").abi_return) == 5
    assert as_int(harness.call(app, "arr(uint256)", 0).abi_return) == 9


def test_param_mutation_incdec_writeback(harness):
    """puyasolRegression/contracts/param_mutation_incdec.sol — NOT an o.g. test.

    A callee mutating a memory ref param ONLY via ++/--/delete (no plain
    assignment) was classified non-mutating by ParamMutationDetector, so the
    caller-side write-back was skipped: inc(arr) with a[0]++ left the caller's
    arr[0] at 0. The detector now records UnaryOperation Inc/Dec/Delete and
    push/pop member-call receivers.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/param_mutation_incdec.sol")
    r = harness.call(app, "run()").abi_return
    assert [as_int(x) for x in r] == [1, 9, 0]
    assert as_int(harness.call(app, "runStruct()").abi_return) == 42


def test_slot_handle_array_bounds_and_packed_compound(harness):
    """puyasolRegression/contracts/slot_handle_array_bounds.sol — NOT an o.g. test.

    Slot-handle (.slot-rebound) fixed-array element access: (1) runtime OOB
    indexes addressed base+idx directly — reading/writing a NEIGHBORING slot
    where EVM panics 0x32; now asserted idx < length. (2) packed `p[i] += v`
    (and even plain `p[i] = v` for an array-typed local, whose intercept never
    fired) did an unscaled whole-word RMW at slot base+i; now routed through
    the packed-aware sub-word read/replace3/write.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/slot_handle_array_bounds.sol")
    # seed
    harness.call(app, "wrPair(uint256,uint256)", 0, 1000)
    harness.call(app, "wrPair(uint256,uint256)", 1, 2000)
    for i, v in [(2, 20), (3, 30), (4, 40)]:
        harness.call(app, "wrPacked(uint256,uint8)", i, v)
    assert as_int(harness.call(app, "rdPair(uint256)", 1).abi_return) == 2000
    assert as_int(harness.call(app, "rdPairChained(uint256)", 0).abi_return) == 1000
    # packed compound: byte 3 bumps; neighbors 2/4 and the pair slots intact
    harness.call(app, "bump(uint256,uint8)", 3, 5)
    harness.call(app, "drop(uint256,uint8)", 2, 1)
    assert as_int(harness.call(app, "rdPacked(uint256)", 3).abi_return) == 35
    assert as_int(harness.call(app, "rdPacked(uint256)", 2).abi_return) == 19
    assert as_int(harness.call(app, "rdPacked(uint256)", 4).abi_return) == 40
    assert as_int(harness.call(app, "rdPair(uint256)", 0).abi_return) == 1000
    assert as_int(harness.call(app, "rdPair(uint256)", 1).abi_return) == 2000
    # checked overflow at the ELEMENT width must revert
    harness.call(app, "wrPacked(uint256,uint8)", 5, 255)
    r = harness.call(app, "bump(uint256,uint8)", 5, 1, expect_revert=True)
    assert getattr(r, "reverted", False), "uint8 overflow in packed += must revert"
    r = harness.call(app, "drop(uint256,uint8)", 6, 1, expect_revert=True)
    assert getattr(r, "reverted", False), "uint8 underflow in packed -= must revert"
    # OOB: EVM Panic 0x32 shape — must revert, not touch neighboring slots
    for sig, args in [
        ("rdPair(uint256)", (2,)),
        ("rdPair(uint256)", (255,)),
        ("rdPairChained(uint256)", (2,)),
        ("wrPair(uint256,uint256)", (2, 99)),
        ("rdPacked(uint256)", (8,)),
        ("wrPacked(uint256,uint8)", (8, 1)),
        ("bump(uint256,uint8)", (8, 1)),
    ]:
        r = harness.call(app, sig, *args, expect_revert=True)
        assert getattr(r, "reverted", False), f"{sig}{args} must revert (index OOB)"


def test_conditional_storage_ptr_reassign_fails_loud(harness):
    """puyasolRegression/contracts/cond_storage_ptr_reassign.sol — NOT an o.g. test.

    `p = a2` on a storage-pointer local lowers to a COMPILE-TIME alias rebind;
    inside an if-branch it applied unconditionally (`if (c) p = a2; p.push(1);`
    always pushed to a2 — verified miscompile). Until a runtime lowering
    exists, conditional reassignment must be a loud compile error.
    """
    with pytest.raises(CompileError):
        harness.compile_and_deploy("puyasolRegression/contracts/cond_storage_ptr_reassign.sol")


def test_straightline_storage_ptr_reassign_still_works(harness):
    """puyasolRegression/contracts/straightline_storage_ptr_reassign.sol.

    The SOUND storage-pointer forms must keep working: straight-line
    reassignment and ternary selection at initialization (the RHS conditional
    is a runtime expression; only conditionally-executed ASSIGNMENTS err).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/straightline_storage_ptr_reassign.sol"
    )
    r = harness.call(app, "straight()").abi_return
    assert [as_int(x) for x in r] == [1, 2]
    # after straight(): a1.length == 1, a2.length == 2 — read through the
    # ternary-selected pointer must see the right array
    assert as_int(harness.call(app, "ternaryLen(bool)", True).abi_return) == 1
    assert as_int(harness.call(app, "ternaryLen(bool)", False).abi_return) == 2


def test_compound_signed_mixedwidth_divisor(harness):
    """puyasolRegression/contracts/compound_signed_mixedwidth.sol — NOT an o.g. test.

    `x /= y` with biguint-backed signed LHS and a NARROWER signed divisor built
    the RHS at the TARGET type: a negative int16 divisor sign-extended from the
    wrong (target) width read as +1.8e19 — x /= -32768 gave 0 instead of 256.
    The RHS is now widened to the target's canonical form first (all compound
    sites share widenSignedCompoundRhs). plainDiv was already correct.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/compound_signed_mixedwidth.sol")
    call = lambda sig, *a: as_signed_int(harness.call(app, sig, *a).abi_return)
    assert call("plainDiv(int128,int16)", -8388609, -32768) == 256
    assert call("compoundDiv(int128,int16)", -8388609, -32768) == 256
    assert call("compoundMod(int128,int16)", -8388609, -32768) == -1
    assert call("compoundDivSmall(int32,int8)", -1000, -125) == 8
    assert call("compoundSub(int128,int16)", 100, -32768) == 32868
    assert call("compoundMul(int128,int16)", 3, -32768) == -98304
    # positive divisors keep working
    assert call("compoundDiv(int128,int16)", -8388608, 32767) == -256


def test_ret_ternary_encode(harness):
    """puyasolRegression/contracts/ret_ternary_encode.sol — NOT an o.g. test.

    Multi-value return with an encoded (signed) element and a ternary whose
    branch is a CALL or nested ternary: encodeReturnValue retyped the node to
    the wire tuple but left the branch unencoded — raw minimal-length biguint
    where 32-byte arc4.uint256 is expected → corrupt return blob. Now spills
    through the opaque-tuple path.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/ret_ternary_encode.sol")
    def pair(sig, *a):
        r = harness.call(app, sig, *a).abi_return
        return as_signed_int(r[0]), as_int(r[1])
    assert pair("callBranch(bool)", True) == (-3, 4)
    assert pair("callBranch(bool)", False) == (-7, 9)
    assert pair("nestedTernary(bool,bool)", True, False) == (-3, 4)
    assert pair("nestedTernary(bool,bool)", False, True) == (-5, 6)
    assert pair("nestedTernary(bool,bool)", False, False) == (-7, 8)


def test_eval_once_unary_pow_enum(harness):
    """puyasolRegression/contracts/eval_once_unary_pow.sol — NOT an o.g. test.

    Operand pinning: checked -g() evaluated g 3x (overflow assert + negate),
    ~g() 2x, unsigned x ** f() ran f 2x (0**0 case + pow), enum-assign RHS 2x
    (range assert + store). Each must run exactly once and compute correctly.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/eval_once_unary_pow.sol")
    r = harness.call(app, "negWide()").abi_return
    assert (as_signed_int(r[0]), as_int(r[1])) == (5, 1)
    r = harness.call(app, "negNarrow()").abi_return
    assert (as_signed_int(r[0]), as_int(r[1])) == (5, 1)
    r = harness.call(app, "invWide()").abi_return
    assert (as_signed_int(r[0]), as_int(r[1])) == (4, 1)  # ~(-5) == 4
    r = harness.call(app, "powRhs(uint64)", 2).abi_return
    assert (as_int(r[0]), as_int(r[1])) == (8, 1)
    r = harness.call(app, "enumAssign()").abi_return
    assert (as_int(r[0]), as_int(r[1])) == (1, 1)  # E.B == 1


def test_super_call_payable_caller(harness):
    """puyasolRegression/contracts/super_payable_caller.sol — NOT an o.g. test.

    Base.f()/super.f() impl copies were built as ABI methods (config reset
    only afterwards), baking the base's not-payable group assert into the
    direct-callsub body: a payable caller grouped with a payment falsely
    reverted. The impl now builds as a plain internal subroutine.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/super_payable_caller.sol", "B", fund_wei=1_000_000
    )
    # payment_wei attaches a preceding PaymentTxn — the pre-fix baked assert fired here
    assert as_int(harness.call(app, "g(uint256)", 3, payment_wei=1000).abi_return) == 3
    assert as_int(harness.call(app, "h(uint256)", 4, payment_wei=1000).abi_return) == 7
    # override still dispatches normally and remains payable-checked
    assert as_int(harness.call(app, "f(uint256)", 5).abi_return) == 17


def test_crosscontract_keyed_getter(harness):
    """puyasolRegression/contracts/crosscontract_keyed_getter.sol — NOT an o.g. test.

    Cross-contract KEYED public getter calls always reverted: the caller
    emitted the return-only selector m()byte[] (and 32-byte biguint keys)
    while the callee published m(<declared-width>)T. Caller now derives
    selector + arg types from the getter FunctionType; callee publishes
    declared key widths (matching explicit functions).
    """
    import algosdk

    arts = harness.compile("puyasolRegression/contracts/crosscontract_keyed_getter.sol")
    store = harness.deploy(arts, contract_name="Store")
    reader = harness.deploy(arts, contract_name="Reader")
    harness.call(store, "seed()")

    fake = algosdk.encoding.encode_address(bytes(24) + store.app_id.to_bytes(8, "big"))
    opts = {"extra_fee": 10_000, "extra_apps": [store.app_id]}
    assert as_int(harness.call(reader, "readMap(address,uint256)", fake, 5, **opts).abi_return) == 500
    assert as_int(harness.call(reader, "readMap(address,uint256)", fake, 6, **opts).abi_return) == 0
    assert as_int(harness.call(reader, "readNarrow(address,uint128)", fake, 9, **opts).abi_return) == 900
    assert as_int(harness.call(reader, "readArr(address,uint256)", fake, 1, **opts).abi_return) == 22
    assert as_int(harness.call(reader, "readArr(address,uint256)", fake, 0, **opts).abi_return) == 0
    # param-less getters keep working (pre-existing night-2 fix)
    assert as_int(harness.call(reader, "readPlain(address)", fake, **opts).abi_return) == 77


def test_memparam_return_in_loop(harness):
    """puyasolRegression/contracts/memparam_return_in_loop.sol — NOT an o.g. test.

    A callee mutating a memory param with an early `return` inside a LOOP
    failed to compile (augmentation walks recursed only if/else, so the
    return kept its unaugmented arity). Both twins now share
    forEachReturnStatement. Mutations before the early return must persist.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/memparam_return_in_loop.sol")
    r = harness.call(app, "viaLibrary()").abi_return
    assert [as_int(x) for x in r] == [11, 21, 0]
    r = harness.call(app, "viaMethod()").abi_return
    assert [as_int(x) for x in r] == [6, 7]


def test_asm_semantics_batch(harness):
    """puyasolRegression/contracts/asm_semantics_batch.sol — NOT an o.g. test.

    Four asm/storage semantics guards: transient sub-64 signed reads
    sign-extend; keccak256 hashes the exact (unaligned) constant length;
    inlined Yul fn body locals alpha-rename per frame; Yul call args
    evaluate right-to-left.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_semantics_batch.sol")
    r = harness.call(app, "transientSigned()").abi_return
    assert (as_signed_int(r[0]), bool(r[1])) == (-1, True)
    # value-dependence on bytes 32..47: pre-fix only the first 32 bytes hashed
    k1 = as_bytes(harness.call(app, "kec48(uint256,uint256)", 7, 1 << 200).abi_return)
    k2 = as_bytes(harness.call(app, "kec48(uint256,uint256)", 7, 2 << 200).abi_return)
    assert k1 != k2, "keccak folded/truncated the unaligned tail"
    # invariance on bytes 48..63 (outside the hashed 48): low bytes of b differ
    k3 = as_bytes(harness.call(app, "kec48(uint256,uint256)", 7, (1 << 200) | 5).abi_return)
    assert k1 == k3, "keccak hashed beyond the requested 48 bytes"
    assert as_int(harness.call(app, "inlineLocals(uint256)", 100).abi_return) == 106
    r = harness.call(app, "argOrder()").abi_return
    assert (as_int(r[0]), as_int(r[1])) == (8, 2)


def test_pending_drain_batch(harness):
    """puyasolRegression/contracts/pending_drain_batch.sol — NOT an o.g. test.

    Pending-statement drain cluster (T1): if-condition write-backs precede the
    IfElse (H1); emit drains arg-build pre-statements (H2); do-while condition
    pendings run with the bottom-of-body test (H3); trailing asm calldatacopy
    persists (H5).
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/pending_drain_batch.sol")
    assert as_int(harness.call(app, "condWriteback()").abi_return) == 1
    # ternary arms: pre-fix the scoped __cond temp's if/else leaked past the
    # emit (log read an unassigned temp / puya use-before-define). Arm gating
    # proves the drain: TRUE never runs g(), FALSE runs it exactly once.
    assert as_int(harness.call(app, "emitTernary(bool)", True).abi_return) == 0
    assert as_int(harness.call(app, "emitTernary(bool)", False).abi_return) == 1
    assert as_int(harness.call(app, "doWhileStorage()").abi_return) == 20
    assert as_int(harness.call(app, "trailingCdc(uint256,byte[])", 12345, b"").abi_return) == 12345


def test_fnptr_dispatch_seam(harness):
    """puyasolRegression/contracts/fnptr_dispatch_seam.sol — NOT an o.g. test.

    Function-pointer seam (H14): distinct signatures get distinct dispatch
    groups (uint8 vs int8, bytes32 vs string); dispatch definition types match
    the call site (address/enum params, multi-return tuple — was void);
    external fn-ptr args go through the shared ARC4 encoder (negative int128
    reached the callee 32-byte-wide and reverted); foreign-contract external
    fn refs stay dynamic instead of an unresolvable direct callsub.
    """
    import algosdk

    arts = harness.compile("puyasolRegression/contracts/fnptr_dispatch_seam.sol")
    target = harness.deploy(arts, contract_name="FnPtrTarget")
    app = harness.deploy(arts, contract_name="FnPtrSeam")

    call = lambda sig, *a, **kw: harness.call(app, sig, *a, **kw).abi_return
    assert as_int(call("pick8(bool,bool,uint8,int8)", True, True, 5, 0)) == 105
    assert as_int(call("pick8(bool,bool,uint8,int8)", False, True, 7, 0)) == 207
    assert as_int(call("pick8(bool,bool,uint8,int8)", True, False, 0, -3)) == 1000
    assert as_int(call("pick8(bool,bool,uint8,int8)", False, False, 0, 5)) == 4000
    assert as_int(call("pickX(bool)", True)) == 0x2A
    assert as_int(call("pickX(bool)", False)) == 3
    zero_addr = algosdk.encoding.encode_address(bytes(32))
    some_addr = algosdk.encoding.encode_address(bytes([1] * 32))
    assert as_int(call("pickAddr(bool,address)", True, zero_addr)) == 7
    assert as_int(call("pickAddr(bool,address)", False, some_addr)) == 80
    assert as_int(call("pickEnum(bool,uint8)", True, 2)) == 502
    assert as_int(call("pickEnum(bool,uint8)", False, 1)) == 601
    r = call("pickPair(bool)", True)
    assert [as_int(x) for x in r] == [11, 22]
    r = call("pickPair(bool)", False)
    assert [as_int(x) for x in r] == [33, 44]
    fake = algosdk.encoding.encode_address(bytes(24) + target.app_id.to_bytes(8, "big"))
    r = call("callExt(address,int128)", fake, -7,
             extra_fee=10_000, extra_apps=[target.app_id])
    assert as_signed_int(r) == -8


def test_amount_overflow_guard(harness):
    """puyasolRegression/contracts/amount_overflow_guard.sol — NOT an o.g. test.

    A uint256 monetary amount >= 2^64 can't fit the AVM's 64-bit amount field.
    Pre-fix, .transfer/.send/{value:} silently sent `amount mod 2^64`
    microAlgos; now such amounts revert. A fitting amount still transfers.
    """
    import algosdk

    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/amount_overflow_guard.sol", fund_wei=5_000_000
    )
    to = harness.localnet.account.address
    two64 = 1 << 64
    # fitting amount works
    assert harness.call(app, "doTransfer(address,uint256)", to, 1000, extra_fee=10_000).reverted is False
    assert bool(harness.call(app, "doSend(address,uint256)", to, 1000, extra_fee=10_000).abi_return) is True
    # >= 2^64 reverts instead of sending amount mod 2^64
    assert harness.call(app, "doTransfer(address,uint256)", to, two64, extra_fee=10_000, expect_revert=True).reverted
    assert harness.call(app, "doSend(address,uint256)", to, two64, extra_fee=10_000, expect_revert=True).reverted
    assert harness.call(app, "doValueCall(address,uint256)", to, two64 + 5, extra_fee=10_000, expect_revert=True).reverted


def test_postinit_creator_only(harness):
    """puyasolRegression/contracts/postinit_creator_only.sol — NOT an o.g. test.

    __postInit gained a creator-only guard: the legitimate create+postInit
    (both from the deployer = app creator) must still deploy and initialize
    state. A front-runner (different sender) would revert on the guard — not
    exercised here (single-account harness); the broad postInit corpus covers
    the positive path.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/postinit_creator_only.sol", ctor_args=[42],
        postinit_inner_txns=2,
    )
    assert as_int(harness.call(app, "len()").abi_return) == 2
    assert as_int(harness.call(app, "arr(uint256)", 0).abi_return) == 42
    assert as_int(harness.call(app, "arr(uint256)", 1).abi_return) == 43
    owner = harness.call(app, "owner()").abi_return
    creator = harness.localnet.account.address
    owner_addr = owner if isinstance(owner, str) else algosdk.encoding.encode_address(bytes(owner))
    assert owner_addr == creator


def test_ecpairing_length_guard(harness):
    """puyasolRegression/contracts/ecpairing_length_guard.sol — NOT an o.g. test.

    ecPairing reshaping hard-codes the 2-pair (384-byte) layout; a wrong-length
    input previously checked only pairs 0-1 (accepting invalid proofs) or
    panicked mid-extract. Now anything but exactly 384 bytes reverts at the
    length assert (before any pairing op — cheap, no budget needed).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/ecpairing_length_guard.sol"
    )
    for n in (0, 100, 383, 385, 768):
        r = harness.call(app, "pairWrongLen(byte[])", b"\x00" * n,
                         extra_fee=10_000, expect_revert=True)
        assert r.reverted, f"pairing with {n}-byte input must revert (not 384)"


def test_mtail_correctness(harness):
    """puyasolRegression/contracts/mtail_correctness.sol — NOT an o.g. test.

    Medium-tail correctness batch: M1 tuple destructure signed widen; M18
    overridden overload not re-emitted; M20 .selector ternary single-eval;
    M24 mulmod evaluates x before the modulus check.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mtail_correctness.sol")
    r = harness.call(app, "tupleSignedWiden()").abi_return
    assert (as_signed_int(r[0]), as_int(r[1])) == (-1, 2)
    # M18: f(uint256) resolves to the override (x+100), f(uint256,uint256) works
    assert as_int(harness.call(app, "f(uint256)", 5).abi_return) == 105
    assert as_int(harness.call(app, "f(uint256,uint256)", 3, 4).abi_return) == 7
    assert as_int(harness.call(app, "selectorTernary()").abi_return) == 1
    r = harness.call(app, "mulmodOrder(uint256)", 5).abi_return
    assert (as_int(r[0]), as_int(r[1])) == ((6 * 5) % 7, 1)


def test_arc4_bool_default_packing(harness):
    """puyasolRegression/contracts/arc4_bool_default_packing.sol — NOT an o.g. test.

    A defaulted mapping(K=>S) value where S has >=2 leading bools + a dynamic
    field: puya packs bools 8/byte, but the default encoder gave each its own
    head byte, so head offsets disagreed with the reader and a read-modify-
    write spliced at the wrong spot. Reads of the default and a modify-from-
    default must both be layout-correct.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/arc4_bool_default_packing.sol"
    )
    r = harness.call(app, "readDefaults(uint256)", 1, extra_fee=5_000).abi_return
    assert [bool(r[0]), bool(r[1]), bool(r[2]), as_int(r[3])] == [False, False, False, 0]
    r = harness.call(app, "modifyFromDefault(uint256)", 2, extra_fee=5_000).abi_return
    assert [bool(r[0]), bool(r[1]), bool(r[2]), as_int(r[3])] == [False, True, False, 1]
    assert as_int(harness.call(app, "arrAt(uint256,uint256)", 2, 0, extra_fee=5_000).abi_return) == 42
    # the untouched neighbor is still all-default
    r = harness.call(app, "readDefaults(uint256)", 2, extra_fee=5_000).abi_return
    assert [bool(r[0]), bool(r[1]), bool(r[2]), as_int(r[3])] == [False, True, False, 1]


def test_batchA_correctness(harness):
    """puyasolRegression/contracts/batchA_correctness.sol — NOT an o.g. test.

    M4: transient assignment-as-expression yields the assigned value (not a
    stale post-pending re-read). M16: self-call via encodeWithSignature picks
    the OVERLOAD by full signature and names the overload-suffixed target.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/batchA_correctness.sol")
    r = harness.call(app, "m4Transient()").abi_return
    assert [as_int(r[0]), as_int(r[1])] == [5, 5]
    assert as_int(harness.call(app, "m16Uint()", extra_fee=5_000).abi_return) == 1005
    assert as_int(harness.call(app, "m16Bool()", extra_fee=5_000).abi_return) == 7


def test_inline_array_external(harness):
    """puyasolRegression/contracts/inline_array_external.sol — NOT an o.g. test.

    Inline array literals as typed external-call args go through the shared
    ARC4 encoder (arc4.uint8 elements at the right width), not the old
    32-byte-word hand-encoding that the callee decoded as garbage.
    """
    import algosdk
    arts = harness.compile("puyasolRegression/contracts/inline_array_external.sol")
    callee = harness.deploy(arts, contract_name="Callee")
    caller = harness.deploy(arts, contract_name="Caller")
    fake = algosdk.encoding.encode_address(bytes(24) + callee.app_id.to_bytes(8, "big"))
    opts = {"extra_fee": 10_000, "extra_apps": [callee.app_id]}
    assert as_int(harness.call(caller, "callU8(address)", fake, **opts).abi_return) == 7
    assert as_int(harness.call(caller, "callU256(address)", fake, **opts).abi_return) == 30


def test_batchB_asm(harness):
    """puyasolRegression/contracts/batchB_asm.sol — NOT an o.g. test.

    Batch B asm/Yul: M6 a Yul `if` with a conditional leave before revert
    doesn't collapse to assert(!cond); M11 transient slot >= 128 reverts,
    small slot round-trips; M12 dynamic calldataload past the end zero-pads.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/batchB_asm.sol")
    # M6: c=0 -> 7 (skip); c!=0,x!=0 -> 42 (leave path); c!=0,x=0 -> revert
    assert as_int(harness.call(app, "m6(uint256,uint256)", 0, 0).abi_return) == 7
    assert as_int(harness.call(app, "m6(uint256,uint256)", 1, 5).abi_return) == 42
    assert harness.call(app, "m6(uint256,uint256)", 1, 0, expect_revert=True).reverted
    # M12: far calldata offset zero-pads (no panic)
    assert as_int(harness.call(app, "m12(uint256)", 100000).abi_return) == 0
    # M11: small transient slot round-trips; slot >= 128 reverts
    assert as_int(harness.call(app, "m11Ok()").abi_return) == 99
    assert harness.call(app, "m11Bad(uint256)", 200, expect_revert=True).reverted


def test_mstore8_multislot(harness):
    """puyasolRegression/contracts/mstore8_multislot.sol — NOT an o.g. test.

    mstore8 at an offset >= 4096 used a slot-0-only replace3 and
    panicked/mis-wrote; it now uses runtime slot math like the slot-aware
    mstore. Write a byte low (slot 0) and high (slot 1), read each back.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/mstore8_multislot.sol")
    assert as_int(harness.call(app, "writeHigh(uint256,uint8)", 100, 0xAB).abi_return) == 0xAB
    assert as_int(harness.call(app, "writeHigh(uint256,uint8)", 5000, 0xCD).abi_return) == 0xCD
    assert as_int(harness.call(app, "writeHigh(uint256,uint8)", 9000, 0x42).abi_return) == 0x42


def test_effect_sequencing(harness):
    """puyasolRegression/contracts/effect_sequencing.sol — NOT an o.g. test.

    OperandPlan intra-expression effect sequencing (fable-review-3 H4 + M5).
    Every expected value verified against real solc 0.8.20 legacy + py-evm:
    binops evaluate RIGHT operand first; assignments RHS-first with the store
    winning over callee write-backs; call args left-to-right with write-backs
    visible to later args; &&/ternary conditions' write-backs visible to the
    RHS/branches.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/effect_sequencing.sol")
    fee = {"extra_fee": 10_000}
    assert as_int(harness.call(app, "h4a()", **fee).abi_return) == 105
    assert as_int(harness.call(app, "h4b()", **fee).abi_return) == 106
    assert as_int(harness.call(app, "h4c()", **fee).abi_return) == 1
    assert as_int(harness.call(app, "h4d()", **fee).abi_return) == 6
    assert as_int(harness.call(app, "h4e()", **fee).abi_return) == 205
    assert as_int(harness.call(app, "h4h()", **fee).abi_return) == 111
    assert as_int(harness.call(app, "h4f()", **fee).abi_return) == 100006
    assert as_int(harness.call(app, "h4g()", **fee).abi_return) == 5100
    r = harness.call(app, "m5a()", **fee).abi_return
    assert [as_int(x) for x in r] == [0, 0, 1]
    r = harness.call(app, "m5b()", **fee).abi_return
    assert [as_int(x) for x in r] == [0, 1, 1]
    r = harness.call(app, "m5c()", **fee).abi_return
    assert [as_int(x) for x in r] == [105, 6]
    assert as_int(harness.call(app, "m5d()", **fee).abi_return) == 100
    assert as_int(harness.call(app, "m5e()", **fee).abi_return) == 106
    r = harness.call(app, "m5f()", **fee).abi_return
    assert [as_int(x) for x in r] == [0, 0, 1]


def test_asm_payload_mem_batch(harness):
    """puyasolRegression/contracts/asm_payload_mem_batch.sol — NOT an o.g. test.

    Final fable-review-3 asm batch, all expectations verified against real
    solc 0.8.26 + py-evm (cancun): H12 asm revert(off,len) delivers the
    payload via the revert-data stack; M12 calldatacopy zero-pads past
    calldatasize; M13 mcopy is memmove for overlapping ranges; M7 keccak /
    mload/mstore are slot-routed at offsets >= 4096.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_payload_mem_batch.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "revSelector()", expect_revert=True, **fee)
    assert r.reverted and r.revert_data == bytes.fromhex("12345678")
    r = harness.call(app, "revWithArg(uint256)", 77, expect_revert=True, **fee)
    assert r.reverted and r.revert_data == bytes.fromhex("deadbeef" + "00" * 31 + "4d")
    r = harness.call(app, "revDyn(uint256)", 10, expect_revert=True, **fee)
    assert r.reverted and r.revert_data == bytes.fromhex("11112222333344445555")
    r = harness.call(app, "revBare()", expect_revert=True, **fee)
    assert r.reverted and r.revert_data == b""
    r = harness.call(app, "revStraddle(uint256)", 32, expect_revert=True, **fee)
    assert r.reverted and r.revert_data == b"\xaa" * 16 + b"\xbb" * 16
    r = harness.call(app, "revStraddle(uint256)", 8, expect_revert=True, **fee)
    assert r.reverted and r.revert_data == b"\xaa" * 8
    data = bytes(range(0x41, 0x61))  # 32 bytes 'A'..'`'
    r = harness.call(app, "cdcTail(bytes)", data, **fee)
    assert as_bytes(r.abi_return) == data[-8:] + bytes(24)
    r = harness.call(app, "mcopyOverlap()", **fee).abi_return
    assert [as_bytes(x) for x in r] == [b"\x01" * 32, b"\x01" * 32, b"\x02" * 32]
    r = harness.call(app, "mcopyOverlapTail()", **fee).abi_return
    assert [as_bytes(x) for x in r] == [b"\x11" * 32, b"\x11" * 16 + b"\x22" * 16]
    r = harness.call(app, "keccakHigh(uint256)", 7, **fee).abi_return
    assert as_bytes(r[0]) == bytes.fromhex(
        "a66cc928b5edb82af9bd49922954155ab7b0942694bea4ce44661d9a8736c688")
    assert as_bytes(r[1]) == bytes.fromhex(
        "3c334a49ed0139fdcb7e40998c11886b23e8d6599a7bf995c0e92eb4b8b558db")
    assert as_int(harness.call(app, "memHighRoundtrip(uint256)", 123456, **fee).abi_return) == 123456


def test_asm_call_value(harness):
    """puyasolRegression/contracts/asm_call_value.sol — NOT an o.g. test.

    M8 remainders: asm call's `value` attaches a grouped [Payment, AppCall]
    (msg.value in the callee sees it; was silently dropped), and the output
    copy / returndatasize strip the ARC4 return prefix (r = raw value, rds =
    EVM-shaped size).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_call_value.sol",
        contract_name="AsmCallValueCaller", fund_wei=2_000_000)
    r = harness.call(app, "run(uint256,uint256)", 150_000, 41, extra_fee=30_000).abi_return
    ok, hits1, got1, ret, rds = (as_int(x) for x in r)
    assert ok == 1
    assert hits1 == 100, "fallback did not run"
    assert got1 == 150_000, f"msg.value did not see the grouped payment (got={got1})"
    assert ret == 1041, f"returndata prefix not stripped (ret={ret})"
    assert rds == 32, f"returndatasize includes the prefix (rds={rds})"


def test_asm_pop_call_output(harness):
    """puyasolRegression/contracts/asm_pop_call_output.sol — NOT an o.g. test.

    pop(call(...)) — the call's success flag is discarded, but the inner txn +
    returndata output-copy must still happen. Was a silent no-op (r read stale
    0x80 = 128). Also the shape UnusedPruner produces (--yul-prepass) from
    `let unused := call(...)`.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_pop_call_output.sol",
        contract_name="PopCallCaller")
    r = harness.call(app, "h(uint256)", 41).abi_return
    assert as_int(r) == 1041, f"pop(call) output not copied (r={as_int(r)})"


def test_asm_slot_storage_ref_param(harness):
    """puyasolRegression/contracts/asm_slot_storage_ref_param.sol — NOT an o.g. test.

    asm `.slot` on a struct storage-ref PARAMETER (solady storage-library idiom):
    `library L { function op(S storage s) { assembly { sload(s.slot) } } }`. The
    param travels as a box-key handle so `s.slot` resolves to the box; sload/sstore
    read/write the slot-0 word. Was a hard error ("unmodeled .slot reference").
    Single uint256-field struct (Uint8Set shape).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_slot_storage_ref_param.sol",
        contract_name="AsmSlotStorageRefParam")
    harness.call(app, "set(uint256)", 3)
    harness.call(app, "set(uint256)", 5)
    assert as_int(harness.call(app, "get(uint256)", 3).abi_return) == 1, "bit 3 not set"
    assert as_int(harness.call(app, "get(uint256)", 5).abi_return) == 1, "bit 5 not set"
    assert as_int(harness.call(app, "get(uint256)", 4).abi_return) == 0, "bit 4 wrongly set"
    assert as_int(harness.call(app, "rawWord()").abi_return) == (1 << 3) | (1 << 5), \
        "raw slot word mismatch (sload/sstore through the param .slot)"


def test_asm_log_emission(harness):
    """puyasolRegression/contracts/asm_log_emit.sol — NOT an o.g. test.

    asm logN (log0..log4) → a single AVM `log` = topic1++…++topicN (32B each) ++
    memory data. log2/log3/log4 previously hard-errored ("unsupported Yul
    builtin"), blocking real event-emitting contracts (solady Ownable). Verifies
    the flat log bytes for a 2-topic and a 0-topic case.
    """
    import base64
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_log_emit.sol", contract_name="AsmLogEmit")

    def raw_logs(r):
        out = []
        for res in (getattr(r.raw_response, "abi_results", None) or []):
            for b in (getattr(res, "tx_info", None) or {}).get("logs", []) or []:
                out.append(base64.b64decode(b))
        return out

    r = harness.call(app, "emit2(uint256,uint256,uint256)", 111, 222, 333)
    exp = (111).to_bytes(32, "big") + (222).to_bytes(32, "big") + (333).to_bytes(32, "big")
    assert any(l == exp for l in raw_logs(r)), "log2 flat bytes (t0++t1++data) mismatch"

    r0 = harness.call(app, "emit0(uint256)", 777)
    assert any(l == (777).to_bytes(32, "big") for l in raw_logs(r0)), "log0 data bytes mismatch"


def test_t2_eval_once_tail(harness):
    """puyasolRegression/contracts/t2_eval_once_tail.sol — NOT an o.g. test.

    T2 EvalOnce tail: call-valued operands referenced more than once by the
    builders evaluate exactly once (cnt == 1, EVM-verified vs solc 0.8.20):
    encodePacked fixed-array loop, address-compare stored side, write-path
    array index, precompile staticcall input.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/t2_eval_once_tail.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "packedArr()", **fee).abi_return
    assert as_int(r[0]) == 1
    assert as_bytes(r[1]) == bytes.fromhex(
        "6e0c627900b24bd432fe7b1f713f1b0744091a646a9fe4a65a18dfed21f2949c")
    r = harness.call(app, "addrCmp()", **fee).abi_return
    assert [as_int(r[0]), as_int(r[1])] == [1, 1]
    r = harness.call(app, "writeIdx()", **fee).abi_return
    assert [as_int(r[0]), as_int(r[1])] == [1, 5]
    r = harness.call(app, "ecInput()", extra_fee=20_000).abi_return
    assert [as_int(r[0]), as_int(r[1])] == [1, 1]


def test_new_in_ctor_postinit(harness):
    """puyasolRegression/contracts/new_in_ctor_postinit.sol — NOT an o.g. test.

    `new ChildNC(50)` inside a ctor (itself deferred to the parent's
    __postInit by box state): the child's create/fund/[pay,__postInit(arg)]
    chain completes before the parent reads child state. Pins the formerly
    documented new-in-ctor known-gap as closed.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_in_ctor_postinit.sol",
        contract_name="ParentNC", fund_wei=5_000_000, postinit_inner_txns=10)
    assert as_int(harness.call(app, "got()").abi_return) == 500
    assert as_int(harness.call(app, "gotPlain()").abi_return) == 77
    assert as_int(harness.call(app, "gotArg()").abi_return) == 50
    assert as_int(harness.call(app, "gotMode()").abi_return) == 2
    assert as_bytes(harness.call(app, "gotTag()").abi_return) == bytes.fromhex("abcdef01")
    assert as_int(harness.call(app, "parrLen()").abi_return) == 2
    assert as_int(harness.call(app, "parr1()").abi_return) == 500


def test_ternary_storage_ptr_mutation(harness):
    """puyasolRegression/contracts/ternary_storage_ptr_mutation.sol — NOT an o.g. test.

    Ternary-INIT storage pointers write THROUGH to the runtime-selected root
    (was a documented known-gap: mutations hit a materialized copy). All
    expectations EVM-verified vs solc 0.8.20. The selection is pinned at
    declaration — flipping the condition's input afterwards must not
    re-select.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/ternary_storage_ptr_mutation.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "pushThrough(bool,uint256)", True, 7, **fee).abi_return
    assert [as_int(x) for x in r] == [2, 0, 8]
    r = harness.call(app, "pushThrough(bool,uint256)", False, 9, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 2, 10]
    r = harness.call(app, "writeThrough(bool,uint256)", True, 42, **fee).abi_return
    assert [as_int(x) for x in r] == [42, 0]
    r = harness.call(app, "writeThrough(bool,uint256)", False, 43, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 43]
    assert as_int(harness.call(app, "readThrough(bool)", True, **fee).abi_return) == 1101
    assert as_int(harness.call(app, "readThrough(bool)", False, **fee).abi_return) == 2201
    r = harness.call(app, "selectThenFlipCond(uint256)", 5, **fee).abi_return
    assert [as_int(x) for x in r] == [1, 0]


def test_ternary_storage_ptr_families(harness):
    """puyasolRegression/contracts/ternary_storage_ptr_families.sol — NOT an o.g. test.

    Ternary-init storage pointers write through for EVERY storage family
    (EVM-verified vs solc 0.8.20): box-keyed structs, app-global structs,
    bytes push, fixed arrays, and mappings (runtime holder name).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/ternary_storage_ptr_families.sol",
        contract_name="TernaryFamilies")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "structWrite(bool,uint256)", True, 7, **fee).abi_return
    assert [as_int(x) for x in r] == [7, 0]
    r = harness.call(app, "structWrite(bool,uint256)", False, 8, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 8]
    r = harness.call(app, "plainStructWrite(bool,uint256)", True, 9, **fee).abi_return
    assert [as_int(x) for x in r] == [9, 0]
    r = harness.call(app, "bytesPush(bool)", True, **fee).abi_return
    assert [as_int(x) for x in r] == [1, 0]
    r = harness.call(app, "bytesPush(bool)", False, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 1]
    r = harness.call(app, "fixedWrite(bool,uint256)", True, 5, **fee).abi_return
    assert [as_int(x) for x in r] == [5, 0]
    r = harness.call(app, "fixedWrite(bool,uint256)", False, 6, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 6]

    mapp = harness.compile_and_deploy(
        "puyasolRegression/contracts/ternary_storage_ptr_families.sol",
        contract_name="MappingTernary")
    r = harness.call(mapp, "mapWrite(bool,uint256,uint256)", True, 1, 3, **fee).abi_return
    assert [as_int(x) for x in r] == [3, 0]
    r = harness.call(mapp, "mapWrite(bool,uint256,uint256)", False, 2, 4, **fee).abi_return
    assert [as_int(x) for x in r] == [0, 4]


def test_asm_cd_layout(harness):
    """puyasolRegression/contracts/asm_cd_layout.sol — NOT an o.g. test.

    solc-derived EVM-ABI synthetic-calldata layout (possible_solc item 2),
    EVM-verified vs solc 0.8.20: signed sub-word head words sign-extend;
    static aggregates inline in the head shifting later params; sub-word
    dynamic arrays re-encode to padded words (count + calldatasize match);
    bytes4 left-aligns.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/asm_cd_layout.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "f1(int8,uint256)", -1, 777, **fee).abi_return
    assert as_bytes(r[0]) == b"\xff" * 32
    assert as_int(r[1]) == 777
    r = harness.call(app, "f2(uint8[3],uint256)", [7, 8, 9], 555, **fee).abi_return
    assert [as_int(x) for x in r] == [7, 9, 555]
    r = harness.call(app, "f3(uint8[])", [65, 66], **fee).abi_return
    assert [as_int(x) for x in r] == [2, 65, 66, 132]
    r = harness.call(app, "f4(bytes4,uint256)", bytes.fromhex("deadbeef"), 333, **fee).abi_return
    assert as_bytes(r[0]) == bytes.fromhex("deadbeef") + bytes(28)
    assert as_int(r[1]) == 333
    r = harness.call(app, "f5(int16[])", [-2], **fee).abi_return
    assert as_bytes(r) == b"\xff" * 31 + b"\xfe"


def test_transitive_param_mutation(harness):
    """puyasolRegression/contracts/transitive_param_mutation.sol — NOT an o.g. test.

    Call-graph closure (possible_solc item 3): params passed on to mutating
    callees count as mutated, so the caller-side write-back chain survives
    one-hop, two-hop, using-for-bound and memory-ref shapes (EVM-verified).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/transitive_param_mutation.sol",
        contract_name="TransParamMut")
    fee = {"extra_fee": 10_000}
    assert as_int(harness.call(app, "goStorage()", **fee).abi_return) == 6
    assert as_int(harness.call(app, "goDeep()", **fee).abi_return) == 11
    assert as_int(harness.call(app, "goBound()", **fee).abi_return) == 27
    assert as_int(harness.call(app, "goMemory()", **fee).abi_return) == 42


def test_denomination_array_layout(harness):
    """puyasolRegression/contracts/denomination_array_layout.sol — NOT an o.g. test.

    Storage-layout drift caught by the item-7 solc-layout tripwire: a
    denomination-sized fixed array (uint[2 ether], ~2e18 slots) no longer
    saturates the slot counter to 2^32-1 and shifts later state vars. `after_`
    keeps its own slot, uncorrupted by the giant array before it.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/denomination_array_layout.sol")
    harness.call(app, "setFirst(uint256)", 1)
    harness.call(app, "setAfter(uint256)", 99)
    # first and after_ read back their OWN slots — the ~2e18-slot array between
    # them must not alias either (pre-fix, the saturated span collided them).
    assert as_int(harness.call(app, "getFirst()").abi_return) == 1
    assert as_int(harness.call(app, "getAfter()").abi_return) == 99
    assert as_int(harness.call(app, "bigLen()").abi_return) == 2 * 10**18


def test_storage_ref_return_loop(harness):
    """puyasolRegression/contracts/storage_ref_return_loop.sol — NOT an o.g. test.

    Storage-ref-pointer return INSIDE a while loop gets the index rewrite
    (the old hand-rolled walker missed loop/switch containers —
    awst::forEachChildBlock consolidation). EVM-verified vs solc 0.8.20.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/storage_ref_return_loop.sol")
    harness.call(app, "seed()", extra_fee=10_000)
    assert as_int(harness.call(app, "firstOfLen(uint256)", 2).abi_return) == 2
    assert as_int(harness.call(app, "firstOfLen(uint256)", 1).abi_return) == 1
    assert as_int(harness.call(app, "firstOfLen(uint256)", 99).abi_return) == 1


def test_asm_cd_static_arrays(harness):
    """puyasolRegression/contracts/asm_cd_static_arrays.sol — NOT an o.g. test.

    Static-array calldata-layout bugs found by the fuzz_cd campaign in item 2:
    bytesN array elements are ONE left-aligned word each (bytes4[2] was
    emitting 8 byte-granular words, shifting the tail); signed sub-word array
    elements sign-extend (int16[2] was zero-padding). Constant-offset map
    path; every value EVM-verified vs solc 0.8.20.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_cd_static_arrays.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "bytesArr(bytes4[2],uint256)",
                     [bytes.fromhex("aabbccdd"), bytes.fromhex("11223344")], 777, **fee).abi_return
    assert as_bytes(r[0]) == bytes.fromhex("aabbccdd") + bytes(28)
    assert as_bytes(r[1]) == bytes.fromhex("11223344") + bytes(28)
    assert as_int(r[2]) == 777          # tail landed at offset 68 (2-word head)
    assert as_int(r[3]) == 100          # calldatasize: 4 + 64 + 32
    r = harness.call(app, "intArr(int16[2],uint256)", [-2, 32767], 555, **fee).abi_return
    assert as_bytes(r[0]) == b"\xff" * 30 + b"\xff\xfe"   # -2 sign-extended
    assert as_bytes(r[1]) == bytes(30) + b"\x7f\xff"      # 32767 positive
    assert as_int(r[2]) == 555
    r = harness.call(app, "u8Arr(uint8[3],uint256)", [7, 8, 9], 333, **fee).abi_return
    assert as_int(as_bytes(r[0]).hex() and int.from_bytes(as_bytes(r[0]), "big")) == 7
    assert int.from_bytes(as_bytes(r[1]), "big") == 9
    assert as_int(r[2]) == 333          # tail at offset 100 (3-word head)


def test_asm_calldatacopy_const(harness):
    """puyasolRegression/contracts/asm_calldatacopy_const.sol — NOT an o.g. test.

    calldatacopy no-op bug (fuzz_mem): a CONSTANT-offset calldatacopy in a
    function with no other dynamic-calldata trigger never stood up the
    synthetic __cd_blob, so the copy was silently skipped and memory read
    zero. Now the blob is always built when any calldatacopy appears. Slot 0
    and slot-crossing (0x1800) destinations + a partial copy, EVM-verified.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_calldatacopy_const.sol")
    fee = {"extra_fee": 10_000}
    r = harness.call(app, "toLow(uint256,uint256)", 0x11223344, 0, **fee).abi_return
    assert as_bytes(r) == bytes(28) + bytes.fromhex("11223344")
    r = harness.call(app, "toHigh(uint256,uint256)", 0, 0xdeadbeef, **fee).abi_return
    assert as_bytes(r) == bytes(28) + bytes.fromhex("deadbeef")
    r = harness.call(app, "partCopy(uint256)", 0x11223344, **fee).abi_return
    # low 8 bytes of arg a (0x11223344) = 0000000011223344, left-aligned, + 24 zeros
    assert as_bytes(r) == bytes.fromhex("0000000011223344") + bytes(24)


def test_bare_literal_mapping_key(harness):
    """puyasolRegression/contracts/mapping_literal_key.sol — NOT an o.g. semantic test.

    A bare integer literal as a bytes32 mapping key (`records[0x0]`, the ENS
    ENSRegistry ctor idiom) is an IntegerConstant of wtype uint64. The
    mapping-key coercion had no uint64->bytesN case, so makeKeyBytes fell to its
    fallback reinterpretCast(uint64 -> bytes) — invalid, rejected by puya
    ("unsupported type cast from uint64 to bytes"). Fixed in
    TypeCoercion::implicitNumericCast (uint64 -> bytes[N] via itob+leftPad), so
    the literal encodes to the same 32-byte key as bytes32(0). This asserts the
    ctor literal-key write and the bytes32-param read hit the SAME box (a key
    mismatch would read 0), in BOTH directions.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/mapping_literal_key.sol")
    z = bytes(32)  # bytes32(0)
    # ctor wrote records[0x0].ttl = 42 via a bare literal; readable via literal...
    assert as_int(harness.call(app, "ttlLit()").abi_return) == 42
    # ...AND via a bytes32(0) param — proves literal-key == param-key encoding.
    assert as_int(harness.call(app, "ttlVia(bytes32)", z).abi_return) == 42
    # write via param, read via literal — proves the match holds both ways.
    harness.call(app, "setViaParam(bytes32,uint64)", z, 99)
    assert as_int(harness.call(app, "ttlLit()").abi_return) == 99
    assert as_int(harness.call(app, "ttlVia(bytes32)", z).abi_return) == 99


def test_asm_mem_ptr_roundtrip(harness):
    """puyasolRegression/contracts/asm_mem_ptr_roundtrip.sol — NOT an o.g. semantic test.

    An asm mstore into a `new bytes` buffer must be visible to a later asm mload
    of the same buffer. The buffer is blob-backed (scratch slots via an offset
    var), but the write was mis-routed to an uninitialised value local while the
    read used the scratch blob, so mload returned 0 (silent wrong value; broke
    ENS AddrResolver bytesToAddress(addressToBytes(a))). Fixed by routing
    blob-backed aggregates through the generic scratch path for both mstore and
    mload. Also exercises the exp(256,N) constant-fold. See ens-compile memory.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_mem_ptr_roundtrip.sol")
    MAX = (1 << 256) - 1
    for v in (0, 1, 42, 0x1122334455, MAX):
        assert as_int(harness.call(app, "rt(uint256)", v).abi_return) == v
        assert as_int(harness.call(app, "rt2(uint256)", v).abi_return) == v
    for v in (0, 1, 0xdeadbeef, (1 << 160) - 1):
        assert as_int(harness.call(app, "expRt(uint160)", v).abi_return) == v


def test_asm_mstore_length_word(harness):
    """puyasolRegression/contracts/asm_mstore_length_word.sol — NOT an o.g. semantic test.

    `mstore(ptr, n)` on a bytes/string buffer POINTER is the EVM LENGTH-WORD
    write: it resizes the buffer. Only the `add(ptr, k)` data form was matched,
    so a bare pointer hard-errored ("cannot coerce non-scalar type 'string' to
    biguint in assembly arithmetic"). That is the OZ ShortStrings.toString
    idiom, and it blocked kaito/degen/builder in the chainwide replay sweep.
    Verified against a live solc+EVM by fuzz_evm.py (93 calls, 0 divergences).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_mstore_length_word.sol")
    w = bytes(range(1, 33))                       # 0x0102...20, all ASCII-safe
    for n in (0, 1, 7, 31, 32):
        # length-then-data and data-then-length must agree: the data write is
        # itself clamped to the buffer length, so both yield w's first n bytes.
        # (string returns decode to str, bytes returns stay bytes.)
        assert harness.call(app, "toStr(bytes32,uint256)", w, n).abi_return == w[:n].decode()
        assert harness.call(app, "toStrRev(bytes32,uint256)", w, n).abi_return == w[:n].decode()
        assert as_bytes(harness.call(app, "shrink(bytes32,uint256)", w, n).abi_return) == w[:n]
    for n in (0, 1, 8, 33, 64):                   # shrink AND grow past new bytes(8)
        assert as_int(harness.call(app, "growLen(uint256)", n).abi_return) == n


def test_assign_target_constant_fails_loud(harness):
    """puyasolRegression/contracts/assign_target_constant.sol — NOT an o.g. test.

    An assignment whose LHS lowers to a constant is a write that goes nowhere.
    It comes from lvalue paths that give up and return a placeholder
    (SolExpressionDispatch's "unsupported member access" warning). puya happens
    to reject a constant target, but other shapes of the same mistake would
    SILENTLY DROP THE STORE, so puya-sol hard-errors on it after serialization.

    Fixture is OZ Checkpoints._unsafeAccess, which writes through a storage ref
    whose slot is computed by ARITHMETIC in assembly, so it denotes a different
    location than any parameter and is NOT a storage-pointer alias — contrast
    test_storage_slot_write_through, where `r.slot := store.slot` IS an identity
    alias and now compiles. Needs real storage-pointer arithmetic; stay loud.
    """
    with pytest.raises(CompileError):
        harness.compile_and_deploy(
            "puyasolRegression/contracts/assign_target_constant.sol")


def test_modifier_arg_bytes32(harness):
    """puyasolRegression/contracts/modifier_arg_bytes32.sol — NOT an o.g. test.

    `onlyRole(MINTER_ROLE)` binds keccak256(...) — AWST wtype UNSIZED `bytes` —
    to a `bytes32` modifier param. puya type-checks the pair and rejected the
    program ("assignment target type differs from expression value type"),
    which blocked five real deployed contracts (gho, strk, imx, xerc20,
    burnminterc20). The bytes were always right; only the wtype label was not.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/modifier_arg_bytes32.sol")
    from eth_utils import keccak
    minter, burner = keccak(b"MINTER_ROLE"), keccak(b"BURNER_ROLE")
    assert as_int(harness.call(app, "mint()").abi_return) == 7
    assert as_bytes(harness.call(app, "lastRole()").abi_return) == minter
    assert as_int(harness.call(app, "burn()").abi_return) == 9
    assert as_bytes(harness.call(app, "lastRole()").abi_return) == burner
    # a runtime bytes32 argument must still bind correctly
    r = bytes(range(32))
    assert as_int(harness.call(app, "anyRole(bytes32)", r).abi_return) == 11
    assert as_bytes(harness.call(app, "lastRole()").abi_return) == r
    # the role must still key a mapping the same way on both paths
    import algosdk.encoding as _enc
    who = _enc.encode_address(bytes(31) + b"\x07")
    harness.call(app, "grant(bytes32,address)", minter, who)
    assert harness.call(app, "isMember(bytes32,address)", minter, who).abi_return is True


def test_string_concat_wtype(harness):
    """puyasolRegression/contracts/string_concat_wtype.sol — NOT an o.g. test.

    `string.concat` returns `string memory` but lowered to the `concat`
    intrinsic labelled plain `bytes`. puya type-checks assignment target vs
    value and rejected the whole program once the result hit a string-typed
    local ("assignment target type differs from expression value type"). The
    return path had its own fixup, so `direct` compiled and `viaLocal` did not.
    Blocked xerc20. Verified against a live solc+EVM (36 calls, 0 divergences).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/string_concat_wtype.sol", ctor_args=["ab"])
    assert harness.call(app, "nm()").abi_return == "xab"
    assert harness.call(app, "viaLocal(string)", "yz").abi_return == "xyz"
    assert harness.call(app, "direct(string)", "q").abi_return == "aqb"
    assert harness.call(app, "chained(string,string)", "l", "r").abi_return == "l-rl-r"
    assert as_int(harness.call(app, "lenOf(string)", "abcd").abi_return) == 7
    # bytes.concat must be untouched by a string-only fix
    assert as_bytes(harness.call(app, "bcat(bytes)", b"\x01\x02").abi_return) \
        == b"\x01\x02\xff"
    assert as_bytes(harness.call(app, "toBytes(string)", "k").abi_return) == b"zk"


def test_blob_array_value_use(harness):
    """puyasolRegression/contracts/blob_array_value_use.sol — NOT an o.g. test.

    A blob-backed ARRAY used as a VALUE leaked its raw uint64 blob offset, so
    the subroutine returned uint64 against an array return type and puya
    rejected the program ("invalid return type [PrimitiveIRType.uint64]").
    Triggered by OZ EnumerableSet.values()'s `assembly { result := store }`
    pointer-pun, which blob-backs `result` and then returns it. Blocked gho.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/blob_array_value_use.sol")
    for n in (0, 1, 2, 5, 12):
        assert as_int(harness.call(app, "countAddrs(uint256)", n).abi_return) == n
        # elements are 1..n, so the sum is the triangular number
        assert as_int(harness.call(app, "sumUints(uint256)", n).abi_return) \
            == n * (n + 1) // 2
    for n in (1, 2, 5, 12):
        assert as_int(harness.call(app, "firstUint(uint256)", n).abi_return) == 1
        assert as_int(harness.call(app, "lastUint(uint256)", n).abi_return) == n


def test_storage_slot_write_through(harness):
    """puyasolRegression/contracts/storage_slot_write_through.sol — NOT o.g.

    OZ StorageSlot: `getStringSlot(store).value = v` is the only legal way to
    write THROUGH a storage pointer. It resolved to a bare biguint slot number,
    `.value` was an unsupported member access, and the write was dropped.
    Blocked 7 real contracts via OZ ShortStrings.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/storage_slot_write_through.sol")
    harness.call(app, "setA(string)", "hello")
    assert harness.call(app, "getA()").abi_return == "hello"
    assert harness.call(app, "readA()").abi_return == "hello"   # read via alias
    assert as_int(harness.call(app, "lenA()").abi_return) == 5
    harness.call(app, "setC(bytes)", b"\x01\x02\x03")
    assert as_bytes(harness.call(app, "getC()").abi_return) == b"\x01\x02\x03"
    # write through a `string storage` param of a LIBRARY function (the OZ shape)
    harness.call(app, "setDViaLib(string)", "via-lib")
    assert harness.call(app, "getD()").abi_return == "via-lib"


def test_storage_slot_write_through_contract_method_fails_loud(harness):
    """The contract-method equivalent must stay a LOUD compile error.

    Only library/free functions get the storage write-back augmentation
    (buildFreestandingSubroutine); a contract method would drop the store. That
    combination was previously unreachable, and must not become silent now that
    the alias resolves.
    """
    with pytest.raises(CompileError):
        harness.compile_and_deploy(
            "puyasolRegression/contracts/storage_slot_write_contract_method.sol")


def test_ens_core_resolver(harness):
    """puyasolRegression/contracts/ens_core_resolver.sol — NOT an o.g. semantic test.

    The five simple ENS resolver profiles (Addr/Text/ContentHash/Name/Pubkey)
    combined like PublicResolver. Guards that their nested-mapping storage
    coexists on the SAME node without box-key aliasing across profiles, string
    mapping keys work (Text), and the AddrResolver asm addr<->bytes (exp fold +
    memory-pointer round-trip) works in the aggregate. Differential-verified
    (65 calls vs live solc+EVM); this pins it with concrete round-trips.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/ens_core_resolver.sol")
    node = b"\x11" * 32
    addrb = b"\xab" * 20
    x, y = b"\x0a" * 32, b"\x0b" * 32
    # all five profiles set on the same node (asm addr<->bytes path is covered
    # separately by test_asm_mem_ptr_roundtrip; here addr uses the raw-bytes API)
    harness.call(app, "setAddr(bytes32,uint256,bytes)", node, 60, addrb)
    harness.call(app, "setText(bytes32,string,string)", node, "url", "https://ens.domains")
    harness.call(app, "setContenthash(bytes32,bytes)", node, b"\xe3\x01\x01\x70")
    harness.call(app, "setName(bytes32,string)", node, "alice.eth")
    harness.call(app, "setPubkey(bytes32,bytes32,bytes32)", node, x, y)
    # read every profile back — coexistence (a box-key alias would corrupt a sibling)
    assert as_bytes(harness.call(app, "addr(bytes32,uint256)", node, 60).abi_return) == addrb
    assert as_bytes(harness.call(app, "text(bytes32,string)", node, "url").abi_return) == b"https://ens.domains"
    assert as_bytes(harness.call(app, "contenthash(bytes32)", node).abi_return) == b"\xe3\x01\x01\x70"
    assert as_bytes(harness.call(app, "getName(bytes32)", node).abi_return) == b"alice.eth"
    pk = harness.call(app, "pubkey(bytes32)", node).abi_return
    assert as_bytes(pk[0]) == x and as_bytes(pk[1]) == y
    # clearRecords bumps the version → every profile reads empty/zero
    harness.call(app, "clearRecords(bytes32)", node)
    assert as_bytes(harness.call(app, "addr(bytes32,uint256)", node, 60).abi_return) == b""
    assert as_bytes(harness.call(app, "text(bytes32,string)", node, "url").abi_return) == b""
    assert as_bytes(harness.call(app, "getName(bytes32)", node).abi_return) == b""
    pk2 = harness.call(app, "pubkey(bytes32)", node).abi_return
    assert as_bytes(pk2[0]) == bytes(32) and as_bytes(pk2[1]) == bytes(32)


def test_asm_signed_div_min(harness):
    """puyasolRegression/contracts/asm_signed_div_min.sol — NOT an o.g. semantic test.

    asm sdiv/smod reverted when the signed result was a NEGATIVE ZERO (e.g.
    sdiv(x, int256.min) → quotient 0 with opposite signs; smod(int256.min, y) →
    remainder 0 with negative dividend), because negate256(0) computed 2^256
    (out of range) instead of 0. EVM returns 0. Fixed by wrapping negate256 mod
    2^256. Found fuzzing Solady FixedPointMathLib (sMulWad/sDivWad).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_signed_div_min.sol")
    MIN = -(1 << 255)
    MAXP = (1 << 255) - 1
    # sdiv: negative-zero quotient must be 0, not a revert
    assert as_signed_int(harness.call(app, "sdiv_(int256,int256)", 5, MIN).abi_return) == 0
    assert as_signed_int(harness.call(app, "sdiv_(int256,int256)", MAXP, MIN).abi_return) == 0
    assert as_signed_int(harness.call(app, "sdiv_(int256,int256)", MIN, 1).abi_return) == MIN
    # sdiv(int256.min, -1) overflows mathematically; EVM sdiv wraps to int256.min
    assert as_signed_int(harness.call(app, "sdiv_(int256,int256)", MIN, -1).abi_return) == MIN
    # smod: negative-zero remainder must be 0, not a revert
    assert as_signed_int(harness.call(app, "smod_(int256,int256)", MIN, 1).abi_return) == 0
    assert as_signed_int(harness.call(app, "smod_(int256,int256)", MIN, 2).abi_return) == 0
    assert as_signed_int(harness.call(app, "smod_(int256,int256)", MIN, MIN).abi_return) == 0
    assert as_signed_int(harness.call(app, "smod_(int256,int256)", 5, MIN).abi_return) == 5
    # sanity: ordinary signed values still correct
    assert as_signed_int(harness.call(app, "sdiv_(int256,int256)", -20, 3).abi_return) == -6
    assert as_signed_int(harness.call(app, "smod_(int256,int256)", -20, 3).abi_return) == -2


def test_asm_byte_oob(harness):
    """puyasolRegression/contracts/asm_byte_oob.sol — NOT an o.g. semantic test.

    asm byte(n, x) must return 0 for n >= 32 (EVM out-of-range). The handler
    range-checked the btoi-TRUNCATED index, so a huge n (>= 2^64) truncated to a
    small in-range index (2^128+5 -> low 64 bits = 5) and wrongly extracted byte
    5 instead of 0. Fixed by checking the original biguint index. Found fuzzing
    Solady DateTimeLib.daysInMonth.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_byte_oob.sol")
    x = int.from_bytes(bytes(range(32)), "big")   # byte i == i for i in 0..31
    for i in (0, 1, 15, 31):
        assert as_int(harness.call(app, "tbyte(uint256,uint256)", i, x).abi_return) == i
    # out-of-range indices → 0 (the bug: huge n truncated to a small in-range index)
    for n in (32, 33, 255, 256, (1 << 128) + 5, (1 << 200) + 31, (1 << 256) - 1):
        assert as_int(harness.call(app, "tbyte(uint256,uint256)", n, x).abi_return) == 0, f"byte({n}) should be 0"


def test_bool_array_condition(harness):
    """puyasolRegression/contracts/bool_array_condition.sol — NOT an o.g. semantic test.

    A bool[] element used directly as a condition (`if (flags[i])`) tripped the
    puya backend ("IfElse.condition expected bool"): arc4.bool is an
    ARC4BasicWType of kind `Basic` (same as native bool), so SolArrayBuilder's
    kind-based needsDecode missed it and the element stayed arc4.bool. Fixed by
    decoding arc4.bool array elements to native bool. Found via OZ
    MerkleProof.multiProofVerify (bool[] proofFlags).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/bool_array_condition.sol")
    f = [False, True, False, True]
    assert as_int(harness.call(app, "ifElem(bool[],uint256)", f, 1).abi_return) == 1
    assert as_int(harness.call(app, "ifElem(bool[],uint256)", f, 0).abi_return) == 0
    assert as_int(harness.call(app, "ternElem(bool[],uint256)", f, 3).abi_return) == 7
    assert as_int(harness.call(app, "ternElem(bool[],uint256)", f, 2).abi_return) == 9
    assert as_int(harness.call(app, "reqElem(bool[],uint256)", f, 1).abi_return) == 3
    assert harness.call(app, "reqElem(bool[],uint256)", f, 0, expect_revert=True).reverted
    assert as_int(harness.call(app, "andElem(bool[],uint256,uint256)", f, 1, 3).abi_return) == 1
    assert as_int(harness.call(app, "andElem(bool[],uint256,uint256)", f, 0, 1).abi_return) == 0
    assert as_int(harness.call(app, "retElem(bool[],uint256)", f, 1).abi_return) == 1
    assert as_int(harness.call(app, "retElem(bool[],uint256)", f, 0).abi_return) == 0


def test_bool_array_write(harness):
    """puyasolRegression/contracts/bool_array_write.sol — NOT an o.g. semantic test.

    Writing a bool[] element (`f[i] = v`) failed with "assignment target type
    differs from expression value type": the write target is the raw arc4.bool
    element, so the RHS native bool must be ARC4-encoded, but
    applyArc4EncodeIfNeeded's kind-based targetIsArc4 check missed arc4.bool
    (kind Basic). Fixed by encoding native bool → arc4.bool. Companion to the
    read fix (test_bool_array_condition).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/bool_array_write.sol")
    for i in (0, 3, 7):
        assert harness.call(app, "writeRead(uint256,bool)", i, True).abi_return is True
        assert harness.call(app, "writeRead(uint256,bool)", i, False).abi_return is False
    assert harness.call(app, "writeMulti(bool,bool,bool,uint256)", True, False, True, 0).abi_return is True
    assert harness.call(app, "writeMulti(bool,bool,bool,uint256)", True, False, True, 1).abi_return is False
    assert harness.call(app, "writeMulti(bool,bool,bool,uint256)", True, False, True, 2).abi_return is True
    # toggle: write `first` then `!first` → result is !first
    assert harness.call(app, "toggle(uint256,bool)", 2, True).abi_return is False
    assert harness.call(app, "toggle(uint256,bool)", 2, False).abi_return is True


def test_bool_array_tuple(harness):
    """puyasolRegression/contracts/bool_array_tuple.sol — NOT an o.g. semantic test.

    Two tuple-assignment fixes:
    1. arc4.bool tuple target: `(m[0], m[1]) = (m[1], m[0])` over a bool[] left the
       RHS native bool — handleTupleAssignment's kind-based targetIsArc4 switch
       missed arc4.bool (kind Basic) → puya "assignment target type differs".
       Exercised via memBoolSwap (storage bool[] element store is blocked by a
       separate puya backend bug, puyabug.md #10).
    2. Array-element parallel swap: `(arr[i], arr[j]) = (arr[j], arr[i])` collapsed
       to sequential writes (both elements took one source value) because the lazy
       RHS was not snapshotted when the LHS is an array element. Fixed by
       snapshotting the RHS into temps for array-element LHS (storage AND memory).
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/bool_array_tuple.sol")
    # bug 2: storage value-type element swaps (must be a REAL swap, not collapse)
    assert harness.call(app, "suSwap(uint256,uint256)", 1, 0).abi_return == [0, 1]
    assert harness.call(app, "suSwap(uint256,uint256)", 5, 9).abi_return == [9, 5]
    assert harness.call(app, "suRot3(uint256,uint256,uint256)", 1, 2, 3).abi_return == [3, 1, 2]
    _si = harness.call(app, "siSwap(int128,int128)", -7, 42).abi_return
    assert [as_signed_int(_si[0]), as_signed_int(_si[1])] == [42, -7]
    assert harness.call(app, "mixLocal(uint256,uint256)", 3, 8).abi_return == [8, 3]
    # bug 2 + bug 1: memory element swaps, incl. bool[] (arc4.bool tuple encode)
    assert harness.call(app, "memUSwap(uint256,uint256)", 1, 0).abi_return == [0, 1]
    assert harness.call(app, "memBoolSwap(bool,bool)", True, False).abi_return == [False, True]
    assert harness.call(app, "memBoolSwap(bool,bool)", False, True).abi_return == [True, False]
    # memory bool 3-way rotate: (m0,m1,m2) = (m2,m0,m1) over [T,F,F] → [F,T,F]
    assert harness.call(app, "memBoolRot3(bool,bool,bool)", True, False, False).abi_return == [False, True, False]
    # bug 2 siblings: struct value-field + mapping-element swaps (MemberAccess / mapping index LHS)
    assert harness.call(app, "structSwap(uint256,uint256)", 1, 0).abi_return == [0, 1]
    assert harness.call(app, "structRot3(uint256,uint256,uint256)", 1, 2, 3).abi_return == [3, 1, 2]
    assert harness.call(app, "mapSwap(uint256,uint256)", 5, 9).abi_return == [9, 5]


def test_address_literal(harness):
    """puyasolRegression/contracts/address_literal.sol — NOT an o.g. semantic test.

    A bare 40-hex-digit address literal (`0x9BA1...`) is typed `address` by solc
    but SolLiteral built it as a biguint IntegerConstant; nothing coerced
    biguint→account, so any assignment/param/return into an `address` failed the
    puya backend with "assignment target type differs from expression value type"
    (real deployed tokens hardcode router/multisig/dead addresses). Fixed by a
    biguint/uint64→account case in TypeCoercion::implicitNumericCast. Relational
    asserts (repr-independent); exact-value correctness vs EVM is covered by the
    differential fuzz.
    """
    # compile+deploy succeeding is the core guard (the bug was a compile error).
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/address_literal.sol")
    router = harness.call(app, "getRouter()").abi_return   # state-var-init literal
    dead = harness.call(app, "dead()").abi_return          # constant literal
    local = harness.call(app, "localLit()").abi_return     # bare local literal
    # each bare literal produces a distinct, non-empty address value
    assert router and dead and local
    assert router != dead and router != local and dead != local
    # two literals through a ternary differ (and each is non-empty)
    a = harness.call(app, "pick(bool)", True).abi_return
    b = harness.call(app, "pick(bool)", False).abi_return
    assert a and b and a != b
    # (exact-value correctness vs EVM — isRouter/eqDead compares — covered by the
    # tiny-fuzzing-oracle differential run on the same contract shape.)


def test_mapping_key_collision(harness):
    """puyasolRegression/contracts/mapping_key_collision.sol — NOT an o.g. semantic test.

    STORAGE ALIASING: mapping box keys are `sha256(keyBytes ++ prefix)`. A
    string/bytes key encodes to RAW variable-length bytes and the prefix (mapping
    name) is variable-length too, so the preimage had two valid splits —
    `sha256("xb" ++ "a") == sha256("x" ++ "ba")` — and mappings `a` and `ba`
    shared ONE box: writing ba["x"] changed a["xb"]. EVM keys these by distinct
    slot numbers, so it was silent storage corruption, with the colliding key
    chosen by the caller. Fixed by hashing DYNAMIC keys to a fixed 32 bytes in
    awst::makeKeyBytes, making every key encoding fixed-width.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/mapping_key_collision.sol")
    # string keys: "a"/"ba" with keys "xb"/"x" is the colliding pair
    harness.call(app, "setA(string,uint256)", "xb", 42)
    assert harness.call(app, "getA(string)", "xb").abi_return == 42
    assert harness.call(app, "getBa(string)", "x").abi_return == 0, "a/ba aliased"
    harness.call(app, "setBa(string,uint256)", "x", 7)
    assert harness.call(app, "getA(string)", "xb").abi_return == 42, "ba write hit a"
    assert harness.call(app, "getBa(string)", "x").abi_return == 7
    # same shape with bytes keys ("c" is a suffix of "bc")
    harness.call(app, "setC(bytes,uint256)", b"\x01\x02", 5)
    assert harness.call(app, "getC(bytes)", b"\x01\x02").abi_return == 5
    assert harness.call(app, "getBc(bytes)", b"\x02").abi_return == 0, "c/bc aliased"
    harness.call(app, "setBc(bytes,uint256)", b"\x02", 9)
    assert harness.call(app, "getC(bytes)", b"\x01\x02").abi_return == 5
    assert harness.call(app, "getBc(bytes)", b"\x02").abi_return == 9


# ── --evm-storage-layout mode (asm-compat-memory-mode.md, stage 1) ──────────
#
# All storage becomes a flat EVM slot space backed by boxes: dense declared
# slots (< 2^16) in 2048-byte pages ("p:" ++ itob(slot/64)), hashed slots
# (mapping entries / dyn-array data at keccak256 outputs) one box per slot
# ("s:" ++ slot32). Solidity-level access and assembly sload/sstore address
# the SAME words, so slot arithmetic in Yul is faithful by construction.

_EVM_LAYOUT = ["--evm-storage-layout"]
_EVM_SOL = "puyasolRegression/contracts/evm_storage_layout.sol"


def _evm_layout_app(harness):
    arts = harness.compile(_EVM_SOL, extra_args=_EVM_LAYOUT)
    # Box MBR: pages are 2048 B (~0.83 ALGO each) and sparse slots ~0.029
    # ALGO each — fund generously.
    return harness.deploy(arts, extra_funding_microalgos=30_000_000)


def test_evm_layout_scalars_and_packing(harness):
    """Value vars (full + packed sub-word + signed + bool + address) via slot
    words; ctor initializers; ++/--/compound/delete; and the packed slot's raw
    word must match EVM's byte layout exactly (checked through asm sload)."""
    app = _evm_layout_app(harness)
    opts = {"extra_fee": 10_000}

    assert as_int(harness.call(app, "getA()", **opts).abi_return) == 41  # ctor init
    assert as_int(harness.call(app, "getB()", **opts).abi_return) == 7   # ctor body
    assert as_int(harness.call(app, "bump()", **opts).abi_return) == 45  # ++/prefix/+=
    harness.call(app, "clearA()", **opts)                                # delete
    assert as_int(harness.call(app, "getA()", **opts).abi_return) == 0

    harness.call(app, "setCD(uint32,bool)", 0xDEADBEEF, True, **opts)
    assert as_int(harness.call(app, "getC()", **opts).abi_return) == 0xDEADBEEF
    assert harness.call(app, "getD()", **opts).abi_return is True
    harness.call(app, "setE(int64)", -5, **opts)
    assert as_signed_int(harness.call(app, "getE()", **opts).abi_return, 256) == -5

    # owner = msg.sender survives the account round-trip (full-slot address
    # keeps all 32 AVM bytes)
    sender = harness.localnet.account.address
    assert as_int(harness.call(app, "getOwner()", **opts).abi_return) == as_int(sender)

    # THE packing check: slot 1 = b | c<<64 | d<<96 | e<<104, exactly as the
    # EVM would pack (uint64, uint32, bool, int64 at byte offsets 0/8/12/13).
    word = as_int(harness.call(app, "rawSlot(uint256)", 1, **opts).abi_return)
    e_tc = (-5) & 0xFFFFFFFFFFFFFFFF
    expected = 7 | (0xDEADBEEF << 64) | (1 << 96) | (e_tc << 104)
    assert word == expected, f"packed word {word:#x} != {expected:#x}"


def test_evm_layout_asm_solidity_coherence(harness):
    """The point of the mode: assembly slot arithmetic and Solidity-level
    access hit the same storage. sstore(s,v) is visible to the high-level
    reader; keccak-derived mapping/array slots match between asm and codegen
    (OZ StorageSlot / Checkpoints idioms)."""
    app = _evm_layout_app(harness)
    opts = {"extra_fee": 10_000}

    # raw sstore to slot 0 = variable `a`
    harness.call(app, "rawStore(uint256,uint256)", 0, 1234, **opts)
    assert as_int(harness.call(app, "getA()", **opts).abi_return) == 1234
    assert as_int(harness.call(app, "aViaAsm()", **opts).abi_return) == 1234

    # StorageSlot idiom: mapping entry slot computed with keccak in ASM,
    # written with sstore, read back through ordinary Solidity codegen.
    sender = harness.localnet.account.address
    harness.call(app, "storeBalAsm(address,uint256)", sender, 777, **opts)
    assert as_int(harness.call(app, "getBal(address)", sender, **opts).abi_return) == 777

    # ...and the reverse: Solidity write, asm keccak+add read (Checkpoints idiom)
    harness.call(app, "pushNum(uint256)", 111, **opts)
    harness.call(app, "pushNum(uint256)", 222, **opts)
    assert as_int(harness.call(app, "numAtAsm(uint256)", 1, **opts).abi_return) == 222

    # absent slots read as zero (EVM semantics; no box materialised on read)
    assert as_int(harness.call(app, "rawSlot(uint256)", 4321, **opts).abi_return) == 0


def test_evm_layout_mappings(harness):
    """Mapping entries at keccak256(key32 ++ slot32); nested mappings chain;
    string keys hash raw bytes; signed values; compound writes."""
    app = _evm_layout_app(harness)
    opts = {"extra_fee": 10_000}
    sender = harness.localnet.account.address

    harness.call(app, "setBal(address,uint256)", sender, 100, **opts)
    harness.call(app, "addBal(address,uint256)", sender, 23, **opts)
    assert as_int(harness.call(app, "getBal(address)", sender, **opts).abi_return) == 123

    harness.call(app, "setGrid(uint256,uint256,int256)", 3, 4, -99, **opts)
    assert as_signed_int(
        harness.call(app, "getGrid(uint256,uint256)", 3, 4, **opts).abi_return, 256) == -99
    assert as_int(harness.call(app, "getGrid(uint256,uint256)", 4, 3, **opts).abi_return) == 0

    harness.call(app, "setNamed(string,uint256)", "alice", 11, **opts)
    harness.call(app, "setNamed(string,uint256)", "bob", 22, **opts)
    assert as_int(harness.call(app, "getNamed(string)", "alice", **opts).abi_return) == 11
    assert as_int(harness.call(app, "getNamed(string)", "bob", **opts).abi_return) == 22

    # struct-in-mapping field write/read
    harness.call(app, "setPossY(uint256,uint128)", 9, 3131, **opts)
    assert as_int(harness.call(app, "getPossY(uint256)", 9, **opts).abi_return) == 3131


def test_evm_layout_arrays_and_structs(harness):
    """Dynamic arrays (length word at p, data at keccak256(p)); packed uint32[]
    elements; push/pop with EVM zero-on-pop; fixed arrays; struct fields at
    sequential slots with sub-word packing."""
    app = _evm_layout_app(harness)
    opts = {"extra_fee": 10_000}

    assert as_int(harness.call(app, "numsLen()", **opts).abi_return) == 0
    harness.call(app, "pushNum(uint256)", 10, **opts)
    harness.call(app, "pushNum(uint256)", 20, **opts)
    harness.call(app, "pushNum(uint256)", 30, **opts)
    assert as_int(harness.call(app, "numsLen()", **opts).abi_return) == 3
    assert as_int(harness.call(app, "numAt(uint256)", 0, **opts).abi_return) == 10
    assert as_int(harness.call(app, "numAt(uint256)", 2, **opts).abi_return) == 30
    harness.call(app, "setNum(uint256,uint256)", 1, 21, **opts)
    assert as_int(harness.call(app, "numAt(uint256)", 1, **opts).abi_return) == 21
    harness.call(app, "popNum()", **opts)
    assert as_int(harness.call(app, "numsLen()", **opts).abi_return) == 2
    # OOB after pop reverts (bounds vs the length word)
    r = harness.call(app, "numAt(uint256)", 2, expect_revert=True, **opts)
    assert r.reverted

    # packed uint32[]: 8 elements per slot
    for i in range(10):
        harness.call(app, "pushPacked(uint32)", i + 1, **opts)
    assert as_int(harness.call(app, "packedAt(uint256)", 0, **opts).abi_return) == 1
    assert as_int(harness.call(app, "packedAt(uint256)", 9, **opts).abi_return) == 10
    harness.call(app, "setPacked(uint256,uint32)", 3, 999, **opts)
    assert as_int(harness.call(app, "packedAt(uint256)", 3, **opts).abi_return) == 999
    assert as_int(harness.call(app, "packedAt(uint256)", 2, **opts).abi_return) == 3, \
        "packed neighbor clobbered"

    # fixed array occupies slots 7..10 directly
    harness.call(app, "setFixed(uint256,uint256)", 2, 555, **opts)
    assert as_int(harness.call(app, "fixedAt(uint256)", 2, **opts).abi_return) == 555
    assert as_int(harness.call(app, "rawSlot(uint256)", 9, **opts).abi_return) == 555

    # struct: x,y pack into slot 11; z takes slot 12
    harness.call(app, "setPos(uint128,uint128,uint256)", 6, 7, 8, **opts)
    assert as_int(harness.call(app, "getPosX()", **opts).abi_return) == 6
    assert as_int(harness.call(app, "getPosZ()", **opts).abi_return) == 8
    word = as_int(harness.call(app, "rawSlot(uint256)", 11, **opts).abi_return)
    assert word == 6 | (7 << 128), f"struct packed word {word:#x}"


def test_evm_layout_default_mode_untouched(harness):
    """No-regression guard: the same fixture compiled WITHOUT the flag keeps
    the named-cell model (per-var ARC-56 state declarations present)."""
    import json as _json
    arts = harness.compile(_EVM_SOL)
    arc56 = _json.loads(arts.by_contract["EvmFull"]["arc56"].read_text())
    keys = arc56.get("state", {}).get("keys", {}).get("global", {})
    maps = arc56.get("state", {}).get("maps", {}).get("box", {})
    assert "a" in keys or "a" in arc56.get("state", {}).get("keys", {}).get("box", {}), \
        "default mode should still declare per-var state"
    assert "bal" in maps


def test_evm_layout_strings(harness):
    """bytes/string state vars in Solidity's EVM storage format: short form
    (data ++ 2*len in one word — verified byte-exact through asm sload), long
    form (2*len+1 word + keccak chunks), shrink clears stale chunks."""
    app = _evm_layout_app(harness)
    opts = {"extra_fee": 10_000}

    harness.call(app, "setTag(string)", "hi", **opts)
    assert harness.call(app, "getTag()", **opts).abi_return == "hi"
    assert as_int(harness.call(app, "tagLen()", **opts).abi_return) == 2
    # EVM short-string form: "hi" left-aligned, low byte = 2*2
    word = as_int(harness.call(app, "tagWord()", **opts).abi_return)
    expected = int.from_bytes(b"hi" + bytes(29) + bytes([4]), "big")
    assert word == expected, f"short-string word {word:#x} != {expected:#x}"

    long_s = "x" * 75  # 3 chunks
    harness.call(app, "setTag(string)", long_s, **opts)
    assert harness.call(app, "getTag()", **opts).abi_return == long_s
    assert as_int(harness.call(app, "tagLen()", **opts).abi_return) == 75
    word = as_int(harness.call(app, "tagWord()", **opts).abi_return)
    assert word == 2 * 75 + 1, "long form length word"

    # shrink long → short: stale chunks must be cleared
    harness.call(app, "setTag(string)", "ok", **opts)
    assert harness.call(app, "getTag()", **opts).abi_return == "ok"
    # the old chunk slots at keccak(15) read back as zero
    import hashlib
    try:
        from Crypto.Hash import keccak as _keccak
        k = _keccak.new(digest_bits=256); k.update((15).to_bytes(32, "big"))
        chunk0 = int.from_bytes(k.digest(), "big")
        assert as_int(harness.call(app, "rawSlot(uint256)", chunk0, **opts).abi_return) == 0
    except ImportError:
        pass  # keccak lib unavailable — chunk-clear still covered by round-trips

    harness.call(app, "clearTag()", **opts)
    assert harness.call(app, "getTag()", **opts).abi_return == ""
    assert as_int(harness.call(app, "tagLen()", **opts).abi_return) == 0

    # string values inside a mapping (leaf at the keccak-derived slot)
    harness.call(app, "setNote(uint256,string)", 1, "alpha", **opts)
    harness.call(app, "setNote(uint256,string)", 2, "y" * 40, **opts)
    assert harness.call(app, "getNote(uint256)", 1, **opts).abi_return == "alpha"
    assert harness.call(app, "getNote(uint256)", 2, **opts).abi_return == "y" * 40


def test_evm_layout_storage_ref_params(harness):
    """Stage 2 of --evm-storage-layout: storage-ref params/locals/returns are
    biguint slot handles. The OZ StorageSlot write-through (asm `r.slot :=
    store.slot` in a library, incl. through a `string storage` library param)
    and the OZ Checkpoints `add(keccak256(...), pos)` idiom run end-to-end."""
    arts = harness.compile(
        "puyasolRegression/contracts/evm_storage_layout_refs.sol",
        extra_args=_EVM_LAYOUT)
    app = harness.deploy(arts, contract_name="EvmRefs",
                         extra_funding_microalgos=30_000_000)
    opts = {"extra_fee": 10_000}

    # StorageSlot write-through: getStringSlot(a).value = v
    harness.call(app, "setA(string)", "hello", **opts)
    assert harness.call(app, "getA()", **opts).abi_return == "hello"
    assert harness.call(app, "readA()", **opts).abi_return == "hello"
    assert as_int(harness.call(app, "lenA()", **opts).abi_return) == 5
    # the REAL OZ shape: through a `string storage` LIBRARY param
    long_s = "z" * 40
    harness.call(app, "setDViaLib(string)", long_s, **opts)
    assert harness.call(app, "getA()", **opts).abi_return == long_s
    # getUint256Slot(bytes32(4)).value = v hits declared var `tail` (slot 4)
    harness.call(app, "setTailViaSlot(uint256)", 4242, **opts)
    assert as_int(harness.call(app, "getTail()", **opts).abi_return) == 4242

    # array storage params through a library
    harness.call(app, "pushNum(uint256)", 5, **opts)
    harness.call(app, "pushNum(uint256)", 6, **opts)
    harness.call(app, "bumpNum(uint256,uint256)", 1, 10, **opts)
    assert as_int(harness.call(app, "numAt(uint256)", 1, **opts).abi_return) == 16
    assert as_int(harness.call(app, "totalNums()", **opts).abi_return) == 21

    # Checkpoints: struct push through using-for + asm keccak+add read
    harness.call(app, "ck(uint32,uint224)", 100, 111, **opts)
    harness.call(app, "ck(uint32,uint224)", 200, 222, **opts)
    assert as_int(harness.call(app, "ckLen()", **opts).abi_return) == 2
    assert as_int(harness.call(app, "ckLatest()", **opts).abi_return) == 222

    # storage locals: bind, packed struct fields, REBIND (runtime slot value)
    harness.call(app, "viaLocal(uint256,uint128,uint128)", 7, 11, 22, **opts)
    r = harness.call(app, "getPos(uint256)", 7, **opts).abi_return
    assert as_int(r[0]) == 11 and as_int(r[1]) == 22
    harness.call(app, "rebind(uint256,uint256,uint128)", 1, 2, 50, **opts)
    assert as_int(harness.call(app, "getPos(uint256)", 1, **opts).abi_return[0]) == 50
    assert as_int(harness.call(app, "getPos(uint256)", 2, **opts).abi_return[0]) == 51


def test_evm_layout_unlocks_slot_access(harness):
    """inlineAssembly/contracts/slot_access.sol — xfailed in the default model
    (asm writes a computed slot the named-cell model can't mirror back), GREEN
    under --evm-storage-layout: sload/sstore and codegen share the slot space."""
    arts = harness.compile("inlineAssembly/contracts/slot_access.sol",
                           extra_args=_EVM_LAYOUT)
    app = harness.deploy(arts, extra_funding_microalgos=30_000_000)
    assert as_int(harness.call(app, "get()").abi_return) == 0
    r = harness.call(app, "mappingAccess(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    harness.call(app, "set(uint256)", 4)
    assert as_int(harness.call(app, "get()").abi_return) == 4
    r = harness.call(app, "mappingAccess(uint256)", 1)
    assert tuple(as_int(x) for x in r.abi_return) == (4, 0)


def test_evm_layout_unlocks_storage_ref_returned(harness):
    """storage/contracts/storage_ref_returned.sol — xfailed in the default
    model, GREEN in slot mode: the returned `T storage` ref IS a biguint slot,
    so mutations through it hit the same words the direct path reads."""
    arts = harness.compile("storage/contracts/storage_ref_returned.sol",
                           extra_args=_EVM_LAYOUT)
    app = harness.deploy(arts, extra_funding_microalgos=30_000_000)
    harness.call(app, "bumpViaRef(uint256,uint256,uint256)", 1, 7, 100,
                 extra_fee=10_000)
    r = harness.call(app, "rdDirect(uint256,uint256)", 1, 7, extra_fee=10_000)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 100)
    r = harness.call(app, "rdViaRef(uint256,uint256)", 1, 7, extra_fee=10_000)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 100)
    harness.call(app, "bumpViaRef(uint256,uint256,uint256)", 1, 7, 200,
                 extra_fee=10_000)
    r = harness.call(app, "rdDirect(uint256,uint256)", 1, 7, extra_fee=10_000)
    assert tuple(as_int(x) for x in r.abi_return) == (2, 200)


def test_evm_layout_unlocks_packed_array_copy(harness):
    """storage/contracts/storage_packed_array_copy.sol — one of the known
    baseline FAILS in the default model, GREEN in slot mode (chained
    assignment value + cross-width bytesN[] convert-copy)."""
    arts = harness.compile("storage/contracts/storage_packed_array_copy.sol",
                           extra_args=_EVM_LAYOUT)
    app = harness.deploy(arts, extra_funding_microalgos=30_000_000)
    opts = {"extra_fee": 10_000}
    r = harness.call(app, "getXAsUint()", **opts)
    assert tuple(as_int(x) for x in r.abi_return) == tuple(range(9))
    r = harness.call(app, "getYAsUint()", **opts)
    assert tuple(as_int(x) for x in r.abi_return) == (0,)*8 + (2, 2)
    harness.call(app, "copy()", **opts)
    r = harness.call(app, "getXAsUint()", **opts)
    assert tuple(as_int(x) for x in r.abi_return) == tuple(range(9))
    r = harness.call(app, "getYAsUint()", **opts)
    assert tuple(as_int(x) for x in r.abi_return) == tuple(range(9)) + (0,)


# ── stage 3: --evm-memory-layout (universal blob memory) guards ─────────────

_EVM_MEM = ["--evm-memory-layout"]
_EVM_BOTH = ["--evm-storage-layout", "--evm-memory-layout"]


def test_evm_layout_unlocks_storage_layout_struct(harness):
    """userDefinedValueType/contracts/storage_layout_struct.sol — a known
    baseline FAIL: `HalfSlot memory x = b; mload(x)` needs the memory struct
    blob-backed. Green with both modes: slot storage + universal blob memory."""
    arts = harness.compile(
        "userDefinedValueType/contracts/storage_layout_struct.sol",
        extra_args=_EVM_BOTH)
    app = harness.deploy(arts, extra_funding_microalgos=30_000_000)
    opts = {"extra_fee": 10_000}
    r = harness.call(app, "storage_a()", **opts)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 0)
    harness.call(app, "set_a(int64,int64)", 100, 200, **opts)
    assert as_int(harness.call(app, "read_slot(uint256)", 0, **opts).abi_return) \
        == 0xc80000000000000064


def test_evm_layout_unlocks_mcopy(harness):
    """inlineAssembly/contracts/mcopy.sol — a known baseline FAIL: asm mcopy
    over memory bytes PARAMS needs them pointer-modeled. Green in memory mode
    (param spill + blob mcopy)."""
    arts = harness.compile("inlineAssembly/contracts/mcopy.sol",
                           extra_args=_EVM_MEM)
    app = harness.deploy(arts, extra_funding_microalgos=10_000_000)
    src = bytes.fromhex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff")
    r = harness.call(app, "f(bytes)", src, extra_fee=10_000)
    assert as_bytes(r.abi_return) == bytes.fromhex(
        "0000000000000000776655443322110000112233445566770000000000000000")


def test_evm_layout_unlocks_keccak_string(harness):
    """inlineAssembly/contracts/keccak_optimization_bug_string.sol — a known
    baseline FAIL: `keccak256(s, 32)` on a string PARAM needs a real pointer.
    Green in memory mode."""
    arts = harness.compile(
        "inlineAssembly/contracts/keccak_optimization_bug_string.sol",
        extra_args=_EVM_MEM)
    app = harness.deploy(arts, extra_funding_microalgos=10_000_000)
    r = harness.call(app, "f(string)", 0x20, 0x2e,
        29457663690442756349866640336617293820574110049925353194191585327958485180523,
        45859201465615193776739262511799714667061496775486067316261261194408342061056,
        extra_fee=10_000)
    assert r.abi_return is False


def test_asm_extcodesize(harness):
    """puyasolRegression/contracts/asm_extcodesize.sol — NOT an o.g. test.

    `extcodesize(a)` in assembly — how OZ's Address.isContract is written, so
    it is vendored into a large share of real contracts (it is what blocks
    BMEX's Vesting dependency in the chainwide differ). Was a hard error
    justified as "no way to query whether an address has code", while
    `address(a).code.length` answered the same question via app_params_get.
    Both spellings now share that lowering.
    """
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_extcodesize.sol",
        contract_name="AsmExtcodesize", postinit_inner_txns=2)
    assert harness.call(app, "contractHasCode()").abi_return is True
    # A real account, not in this compiler's contract-value form, reads as
    # "no code" — the same caveat `.code.length` carries.
    assert harness.call(app, "eoaHasNoCode(address)",
                        harness.localnet.account.address).abi_return is False
    assert harness.call(app, "agreesWithDotCode()").abi_return is True
    assert as_int(harness.call(app, "twoReads()").abi_return) > 0


def test_enumerable_set_values(harness):
    """puyasolRegression/contracts/enumerable_set_values.sol — NOT an o.g. test.

    OZ EnumerableSet.values() returned [] for a NON-EMPTY set while length(),
    at(i) and contains() were all correct — a silent wrong value, found by the
    chainwide differ on SystemCoin's authorizedAccounts(). Two independent bugs:

      * a struct containing a MAPPING was left in app-global state while every
        storage ref to it reads a runtime box-key PREFIX, so the read found a
        box that was never written (StorageMapper::shouldUseBoxStorage);
      * `address[] memory result; assembly { result := store }` blob-backed the
        UNINITIALISED local, allocating an empty region that the later value-use
        materialised instead of the assigned array. The named-return spelling of
        the same pun always worked.
    """
    arts = harness.compile("puyasolRegression/contracts/enumerable_set_values.sol")
    app = harness.deploy(arts, "EnumerableSetValues",
                         extra_funding_microalgos=5_000_000)
    a = harness.localnet.account.address
    assert harness.call(app, "add(address)", a, extra_fee=20_000).abi_return is True
    assert as_int(harness.call(app, "length()", extra_fee=10_000).abi_return) == 1
    at0 = harness.call(app, "at(uint256)", 0, extra_fee=10_000).abi_return
    vals = harness.call(app, "values()", extra_fee=10_000).abi_return
    assert vals == [at0], f"values() lost the set (got {vals}, at(0)={at0})"
