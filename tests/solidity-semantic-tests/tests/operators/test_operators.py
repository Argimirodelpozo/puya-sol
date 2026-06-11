"""Tests for the operators category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes, as_signed_int,
)


def test_compound_assign(harness):
    """operators/contracts/compound_assign.sol"""
    app = harness.compile_and_deploy("operators/contracts/compound_assign.sol")
    # f(uint256,uint256): 0, 6 -> 7
    r = harness.call(app, "f(uint256,uint256)", 0, 6)
    assert as_int(r.abi_return) == 7
    # f(uint256,uint256): 1, 3 -> 0x23
    r = harness.call(app, "f(uint256,uint256)", 1, 3)
    assert as_int(r.abi_return) == 35
    # f(uint256,uint256): 2, 25 -> 0x0746
    r = harness.call(app, "f(uint256,uint256)", 2, 25)
    assert as_int(r.abi_return) == 1862
    # f(uint256,uint256): 3, 69 -> 396613
    r = harness.call(app, "f(uint256,uint256)", 3, 69)
    assert as_int(r.abi_return) == 396613
    # f(uint256,uint256): 4, 84 -> 137228105
    r = harness.call(app, "f(uint256,uint256)", 4, 84)
    assert as_int(r.abi_return) == 137228105
    # f(uint256,uint256): 5, 2 -> 0xcc7c5e28
    r = harness.call(app, "f(uint256,uint256)", 5, 2)
    assert as_int(r.abi_return) == 3430702632
    # f(uint256,uint256): 6, 51 -> 1121839760671
    r = harness.call(app, "f(uint256,uint256)", 6, 51)
    assert as_int(r.abi_return) == 1121839760671
    # f(uint256,uint256): 7, 48 -> 408349672884251
    r = harness.call(app, "f(uint256,uint256)", 7, 48)
    assert as_int(r.abi_return) == 408349672884251

def test_compound_assign_transient_storage(harness):
    """operators/contracts/compound_assign_transient_storage.sol"""
    app = harness.compile_and_deploy("operators/contracts/compound_assign_transient_storage.sol")
    # f(uint256,uint256): 0, 6 -> 7
    r = harness.call(app, "f(uint256,uint256)", 0, 6)
    assert as_int(r.abi_return) == 7
    # f(uint256,uint256): 1, 3 -> 11
    r = harness.call(app, "f(uint256,uint256)", 1, 3)
    assert as_int(r.abi_return) == 11
    # f(uint256,uint256): 2, 25 -> 0x3c
    r = harness.call(app, "f(uint256,uint256)", 2, 25)
    assert as_int(r.abi_return) == 60
    # f(uint256,uint256): 3, 69 -> 0xdc
    r = harness.call(app, "f(uint256,uint256)", 3, 69)
    assert as_int(r.abi_return) == 220
    # f(uint256,uint256): 4, 84 -> 353
    r = harness.call(app, "f(uint256,uint256)", 4, 84)
    assert as_int(r.abi_return) == 353
    # f(uint256,uint256): 5, 2 -> 0x20
    r = harness.call(app, "f(uint256,uint256)", 5, 2)
    assert as_int(r.abi_return) == 32
    # f(uint256,uint256): 6, 51 -> 334
    r = harness.call(app, "f(uint256,uint256)", 6, 51)
    assert as_int(r.abi_return) == 334
    # f(uint256,uint256): 7, 48 -> 371
    r = harness.call(app, "f(uint256,uint256)", 7, 48)
    assert as_int(r.abi_return) == 371

def test_transient_storage_variable_increment_decrement(harness):
    """operators/contracts/transient_storage_variable_increment_decrement.sol"""
    app = harness.compile_and_deploy("operators/contracts/transient_storage_variable_increment_decrement.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1


def test_incdec_side_effect_index(harness):
    """operators/contracts/incdec_side_effect_index.sol

    CUSTOM regression guard (NOT vendored from the upstream Solidity semantic
    suite). An inc/dec whose target has a side-effecting index — `arr[i++]++` —
    must evaluate the index exactly once. Before the fix the write target was
    rebuilt from the raw subexpression, re-running `i++` a second time: the read
    used arr[0] but the write landed on arr[1] and `i` ended at 2.
    Expected EVM semantics: arr[i++]++ with i=0 -> arr=[11,20,30], i=1.
    """
    app = harness.compile_and_deploy("operators/contracts/incdec_side_effect_index.sol")
    assert tuple(as_int(x) for x in harness.call(app, "memArr()").abi_return) == (11, 20, 30, 1)
    assert tuple(as_int(x) for x in harness.call(app, "memArrDec()").abi_return) == (10, 20, 31, 1)


def test_compound_side_effect_index(harness):
    """operators/contracts/compound_side_effect_index.sol

    CUSTOM regression guard (NOT vendored). A compound assignment whose target
    has a side-effecting index — `arr[i++] += 5` — must evaluate the index once.
    Before the fix the current-value read rebuilt the LHS, re-running `i++`: the
    read saw arr[1], the write landed on arr[0] (set to 25), and `i` ended at 2.
    Expected: arr[i++] += 5 with i=0 -> arr=[15,20,30], i=1. Also checks a
    plain-key storage compound still works.

    NOTE: a side-effecting *mapping key* (e.g. `bal[bump()] += 7`) is a separate,
    deeper limitation — the mapping key is re-inlined at every box op rather than
    hoisted to a temp — and is intentionally not covered here.
    """
    app = harness.compile_and_deploy("operators/contracts/compound_side_effect_index.sol")
    assert tuple(as_int(x) for x in harness.call(app, "memCompound()").abi_return) == (15, 20, 30, 1)
    assert as_int(harness.call(app, "stoCompound(uint256)", 3).abi_return) == 107


def test_shortcircuit_side_effect(harness):
    """operators/contracts/shortcircuit_side_effect.sol

    CUSTOM regression guard (NOT vendored). `&&`/`||` must short-circuit even
    when the RHS has side effects or would panic: `false && bump()` must not
    call bump(), `true || bump()` must not call bump(), and `x != 0 && 100/x > 5`
    with x==0 must not divide (no panic). Protects the lazy lowering of
    BooleanBinaryOperation against a regression that eagerly hoists the RHS.
    """
    app = harness.compile_and_deploy("operators/contracts/shortcircuit_side_effect.sol")
    a = harness.call(app, "andSC()").abi_return
    assert (bool(a[0]), as_int(a[1])) == (False, 0)
    o = harness.call(app, "orSC()").abi_return
    assert (bool(o[0]), as_int(o[1])) == (True, 0)
    assert bool(harness.call(app, "divGuard(uint256)", 0).abi_return) is False
    assert bool(harness.call(app, "divGuard(uint256)", 10).abi_return) is True


def test_signed_arith_side_effect_once(harness):
    """operators/contracts/signed_arith_side_effect.sol

    CUSTOM regression guard (NOT vendored). Signed add/sub/mul reference each
    operand multiple times (sign/overflow/range checks alongside the result).
    A side-effecting operand — `a() + b()` where a()/b() bump a counter — must
    execute exactly once per source occurrence. Before the fix each operand ran
    ~4-5x (cnt 9/6/11 instead of 2). Fixed by wrapping signed operands in
    SingleEvaluation AND correcting the serializer to emit `_id` (puya's attrs
    key) so the backend's single-eval cache actually dedups the copies.
    """
    app = harness.compile_and_deploy("operators/contracts/signed_arith_side_effect.sol")
    for fn, val in [("sadd()", 7), ("ssub()", 1), ("smul()", 12)]:
        r = harness.call(app, fn).abi_return
        assert (as_signed_int(r[0]), as_int(r[1])) == (val, 2), f"{fn} -> {r}"


def test_signed_divmodexp_side_effect_once(harness):
    """operators/contracts/signed_divmodexp_side_effect.sol

    CUSTOM regression guard (NOT vendored). Companion to
    test_signed_arith_side_effect_once for the signed div/mod/exp handlers,
    which also reference operands multiply (div-by-zero / INT_MIN/-1 / sign
    checks). `a()/b()`, `a()%b()`, `b()**2` must each run their operand calls
    exactly once. Covered by the same SingleEvaluation wrap (applied before the
    signed-handler dispatch) plus the serializer `_id` fix.
    """
    app = harness.compile_and_deploy("operators/contracts/signed_divmodexp_side_effect.sol")
    for fn, val, ec in [("sdiv()", 3, 2), ("smod()", 2, 2), ("sexp()", 36, 1)]:
        r = harness.call(app, fn).abi_return
        assert (as_signed_int(r[0]), as_int(r[1])) == (val, ec), f"{fn} -> {r}"
