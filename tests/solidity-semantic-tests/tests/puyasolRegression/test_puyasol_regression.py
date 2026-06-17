"""puya-sol-specific regression guards — NOT vendored / NOT original Solidity
semantic tests.

These pin behaviour for bugs fixed in the puya-sol AVM backend itself that the
upstream Solidity semantic-test corpus does not exercise. Kept deliberately
separate from the ported `tests/<cat>/` suites.
"""
from framework import as_int


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
