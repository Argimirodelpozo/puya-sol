"""puya-sol-specific regression guards — NOT vendored / NOT original Solidity
semantic tests.

These pin behaviour for bugs fixed in the puya-sol AVM backend itself that the
upstream Solidity semantic-test corpus does not exercise. Kept deliberately
separate from the ported `tests/<cat>/` suites.
"""
import pytest

from framework import as_int
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
