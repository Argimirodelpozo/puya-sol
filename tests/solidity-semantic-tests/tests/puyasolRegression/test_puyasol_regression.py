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
