"""Custom selector dispatch must gate receive/fallback on OnCompletion.

new_review.md A2: with receive()/fallback() present, the hand-written dispatch
approved bare (NumAppArgs==0) and unmatched-selector calls without reading
Txn.OnCompletion — a 0-argument DeleteApplication/UpdateApplication/CloseOut
entered the receive() arm and returned approval, so anyone could delete or
replace the app. Both arms are NoOp-only now; lifecycle completions fall
through to the ARC-4 router's per-method gating and are rejected.
"""

import base64

import pytest
from algosdk import transaction

from framework import as_int

# Specific rejection only — a broad Exception match would also pass on the
# known algod-30s-timeout flake class.
REJECTED = "rejected by ApprovalProgram|logic eval error"


def _lifecycle_txn(harness, app, artifacts, name, oc, app_args=None):
    """Submit an app call with a non-NoOp OnCompletion (signed by the funded
    localnet account — the point is that NO sender may take these paths)."""
    client = harness.localnet.algod
    acct = harness.localnet.account
    params = client.suggested_params()
    if oc == "delete":
        txn = transaction.ApplicationDeleteTxn(
            acct.address, params, app.app_id, app_args=app_args)
    elif oc == "closeout":
        txn = transaction.ApplicationCloseOutTxn(
            acct.address, params, app.app_id, app_args=app_args)
    elif oc == "update":
        entry = artifacts.by_contract[name]
        approval = base64.b64decode(
            client.compile(entry["approval_teal"].read_text())["result"])
        clear = base64.b64decode(
            client.compile(entry["clear_teal"].read_text())["result"])
        txn = transaction.ApplicationUpdateTxn(
            acct.address, params, app.app_id, approval, clear,
            app_args=app_args)
    else:
        raise ValueError(oc)
    signed = txn.sign(acct.private_key)
    txid = client.send_transaction(signed)
    return transaction.wait_for_confirmation(client, txid, 4)


def test_receive_fallback_oncompletion_gate(harness):
    artifacts = harness.compile(
        "puyasolRegression/contracts/fallback_oncompletion.sol")
    app = harness.deploy(artifacts, "FallbackOC")

    # NoOp entry paths still work: bare → receive(), unmatched → fallback().
    harness.call_bare(app)
    assert as_int(harness.call(app, "hits()").abi_return) == 1
    harness.call_raw(app, b"\xde\xad\xbe\xef")
    assert as_int(harness.call(app, "hits()").abi_return) == 101

    # Bare lifecycle completions must NOT reach receive()/fallback().
    for oc in ("delete", "update", "closeout"):
        with pytest.raises(Exception, match=REJECTED):
            _lifecycle_txn(harness, app, artifacts, "FallbackOC", oc)

    # Unmatched-selector lifecycle completions must not reach fallback() either.
    for oc in ("delete", "update", "closeout"):
        with pytest.raises(Exception, match=REJECTED):
            _lifecycle_txn(harness, app, artifacts, "FallbackOC", oc,
                           app_args=[b"\xde\xad\xbe\xef"])

    # The app survived all six attempts with state intact.
    assert as_int(harness.call(app, "hits()").abi_return) == 101
