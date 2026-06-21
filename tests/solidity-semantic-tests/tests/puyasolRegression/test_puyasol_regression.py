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


@pytest.mark.xfail(reason="puya backend: dynamic STORAGE array of a biguint-backed element narrower "
                          "than 32 bytes (uint128/int128/uint160/...) reports .length as "
                          "total_bytes/32 (a hardcoded 32-byte stride) instead of the element count; "
                          "data is correct. uint256[] cancels (32/32), uint64[] uses a different path. "
                          "Frontend AWST is faithful → puya get_length/box-array. See fuzz_gen stateful "
                          "+ [[differential-fuzzing-spike]]. Remove xfail when puya fixes it.")
def test_wide_dynamic_array_length(harness):
    """puyasolRegression/contracts/wide_dynamic_array_length.sol — NOT an o.g. semantic test.

    Found by the generative STATEFUL fuzzer. A wide (biguint-backed, <32-byte) dynamic STORAGE array's
    `.length` is wrong (uses a 32-byte stride); element DATA is stored/indexed correctly. PUYA BACKEND.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/wide_dynamic_array_length.sol")
    for v in (111, 222, 333):
        harness.call(app, "push(uint128)", v)
    # data is CORRECT (these pass) — only .length is wrong
    assert as_int(harness.call(app, "get(uint256)", 0).abi_return) == 111
    assert as_int(harness.call(app, "get(uint256)", 2).abi_return) == 333
    # the bug: length reads 1 (= 3*16/32) instead of 3
    assert as_int(harness.call(app, "len()").abi_return) == 3


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


@pytest.mark.xfail(reason="frontend: Yul user-defined functions are inline-expanded by binding params/"
                          "returns to BARE names (x, y) in m_locals (UserFunctionOps.cpp ~116-191), so "
                          "functions sharing names — or nested/repeated calls — clobber the same runtime "
                          "vars. `add(sq(a),cube(b))` collapses to 2*a^3 (every call -> cube(a)) not "
                          "a^2+b^3. Fix = unique per-inline names + a scoped rename map in resolveVarRef "
                          "(like the subroutine path's __yulret_<id> temps). See [[differential-fuzzing-spike]].")
def test_yul_user_fn_var_clash(harness):
    """puyasolRegression/contracts/yul_user_fn_var_clash.sol — NOT an o.g. semantic test.

    Found by the generative fuzzer (Yul user functions). Inline-expanded Yul user functions sharing
    param/return names clobber each other's runtime vars. FRONTEND. Verified in the semantic harness.
    """
    app = harness.compile_and_deploy("puyasolRegression/contracts/yul_user_fn_var_clash.sol")
    # uf(a,b) = a^2 + b^3
    assert as_int(harness.call(app, "uf(uint256,uint256)", 2, 0).abi_return) == 4
    assert as_int(harness.call(app, "uf(uint256,uint256)", 3, 2).abi_return) == 17
    assert as_int(harness.call(app, "uf(uint256,uint256)", 5, 3).abi_return) == 52
