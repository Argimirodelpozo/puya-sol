"""Flash accounting via AVM atomic transaction groups (no re-entrancy).

Proves the architecture that lets a Uniswap-V4-style PoolManager work on the
AVM, where re-entrancy is forbidden: instead of unlock()+callback+re-enter, the
caller submits ONE atomic group of top-level calls to the same app
(`[unlock, op, op, settle, ...]`). Deltas accumulate in global state — visible
to later txns in the same group — and the last op asserts they net to zero,
reverting the whole atomic group otherwise.
"""
import os

import pytest
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer,
    AccountTransactionSigner,
)

from framework import as_int
from framework.call import _populate_group_resources_progressively, _resolve_method


def _run_group(harness, app, calls):
    """Execute [("sig", (args,)), ...] as ONE atomic group of app calls.

    Returns (ok, error_text). ok=False means the group was rejected (atomic
    revert — none of its effects persisted).
    """
    ln = harness.localnet
    algod = ln.algod
    sender = ln.account.address
    signer = AccountTransactionSigner(ln.account.private_key)
    atc = AtomicTransactionComposer()
    for sig, args in calls:
        method = _resolve_method(app.app_spec, sig)
        sp = algod.suggested_params()
        sp.flat_fee = True
        sp.fee = 2000
        atc.add_method_call(
            app_id=app.app_id,
            method=method,
            sender=sender,
            sp=sp,
            signer=signer,
            method_args=list(args),
            note=os.urandom(8),  # keep otherwise-identical app calls distinct
        )
    # Slot mode backs storage with boxes — discover and attach refs the way
    # framework.call does (a raw group carries none; default mode is a no-op).
    atc = _populate_group_resources_progressively(atc, algod, "flash-group")
    try:
        atc.execute(algod, 6)
        return True, ""
    except Exception as e:  # noqa: BLE001 — group rejected (atomic revert)
        return False, str(e)


def test_flash_accounting_group(harness):
    """inlineAssembly/contracts/flash_accounting.sol"""
    app = harness.compile_and_deploy("inlineAssembly/contracts/flash_accounting.sol")

    # 1. Balanced group: ops accumulate across the group, last op settles to
    #    zero and closes the lock. The whole thing is one atomic group, with
    #    NO call ever re-entering the app.
    ok, err = _run_group(harness, app, [
        ("unlock()", ()),
        ("op(uint256)", (100,)),
        ("op(uint256)", (30,)),
        ("settle(uint256)", (130,)),
    ])
    assert ok, f"balanced group should succeed: {err}"
    assert harness.call(app, "isUnlocked()").abi_return is False
    assert as_int(harness.call(app, "getNetDelta()").abi_return) == 0

    # 2. Reusable: a second balanced group works after the first closed.
    ok, err = _run_group(harness, app, [
        ("unlock()", ()),
        ("op(uint256)", (50,)),
        ("settle(uint256)", (50,)),
    ])
    assert ok, f"second group should succeed: {err}"
    assert harness.call(app, "isUnlocked()").abi_return is False

    # 3. Under-settled group: last op asserts net!=0 -> the WHOLE atomic group
    #    reverts, so the op(100) "credit" is undone and no funds leak.
    ok, err = _run_group(harness, app, [
        ("unlock()", ()),
        ("op(uint256)", (100,)),
        ("settle(uint256)", (40,)),  # leaves 60 owed
    ])
    assert not ok, "under-settled group must revert"
    # state untouched by the reverted group
    assert harness.call(app, "isUnlocked()").abi_return is False
    assert as_int(harness.call(app, "getNetDelta()").abi_return) == 0

    # 4. Taking on credit with NO settle at all: the last op (op itself)
    #    self-checks net==0 and reverts. You cannot escape settlement.
    ok, err = _run_group(harness, app, [
        ("unlock()", ()),
        ("op(uint256)", (100,)),
    ])
    assert not ok, "unsettled single-op group must revert"
    assert harness.call(app, "isUnlocked()").abi_return is False

    # 5. A lone op() with no unlock is rejected (lock closed).
    ok, err = _run_group(harness, app, [
        ("op(uint256)", (1,)),
    ])
    assert not ok, "op without unlock must revert (LOCKED)"
