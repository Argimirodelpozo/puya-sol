"""End-to-end tests for AccessManagerEnumerable under --uros-splitter.

New 3-contract architecture (main + __storage + orch):
  - main: thin entry point with stubs forwarding to orch via inner txn
  - __storage: state holder; chunks swap onto it per call
  - orch: holds chunk bytecode in boxes, runs install/call/restore dance

User calls main.foo(args) directly — no group dance from caller side.
main's stub does the inner-call to orch, orch dances on __storage.

These tests verify the new architecture works end-to-end on a real
AAVE V4 contract (AME has 28+ split methods across 4 chunks).
"""

from __future__ import annotations

import base64
import os

import algokit_utils as au
import pytest
from algokit_utils.models.account import SigningAccount
from algosdk import encoding

from uros_dance import deploy_split_app, _arc4_selector


def _addr_to_pk32(addr: str) -> bytes:
    return encoding.decode_address(addr)


def test_deploy(localnet: au.AlgorandClient, account: SigningAccount, orch_app_id: int):
    """Smoke: AME deploys via the new 3-contract architecture
    (main + __storage + orch) without errors."""
    d = deploy_split_app(
        localnet.client.algod, account, "AccessManagerEnumerable",
        orch_id=orch_app_id,
        app_args=[_addr_to_pk32(account.address)],
    )
    assert d.main_id > 0
    assert d.storage_id > 0
    assert d.orch_id == orch_app_id


def test_get_role_count_via_dance(
    localnet: au.AlgorandClient, account: SigningAccount, orch_app_id: int
):
    """Call a split view (getRoleCount → uint256) via main; main's
    stub forwards to orch which dances on __storage. Result lands as
    an ABI return log on main's outer call."""
    d = deploy_split_app(
        localnet.client.algod, account, "AccessManagerEnumerable",
        orch_id=orch_app_id,
        app_args=[_addr_to_pk32(account.address)],
    )

    # Call main.getRoleCount() directly. main's stub does the inner-
    # call to orch.dispatch which dances on __storage.
    algod = localnet.client.algod
    sp = algod.suggested_params()
    sp.fee = sp.min_fee * 16  # main → orch → 3-itxn dance + buffer
    from algosdk.transaction import ApplicationCallTxn, OnComplete
    from algosdk.atomic_transaction_composer import (
        AccountTransactionSigner, AtomicTransactionComposer, TransactionWithSigner,
    )
    sel = _arc4_selector("getRoleCount()uint256")
    txn = ApplicationCallTxn(
        sender=account.address, sp=sp, index=d.main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[sel],
        foreign_apps=[d.orch_id, d.storage_id],
        note=os.urandom(8),
    )
    signer = AccountTransactionSigner(account.private_key)
    atc = AtomicTransactionComposer()
    atc.add_transaction(TransactionWithSigner(txn, signer))
    atc = au.populate_app_call_resources(atc, algod)
    result = atc.execute(algod, 4)

    # Find the ABI return log. With the new architecture, main
    # forwards the chunk's last_log via its own log emission.
    txid = result.tx_ids[0]
    confirmation = algod.pending_transaction_info(txid)

    # Collect ALL logs at every nesting depth so we can inspect what
    # main emitted vs. what the chunk emitted (chunk's log is at
    # inner[0]/inner[1], main's is at the outer level).
    all_logs = []
    def collect_logs(node, path=""):
        if isinstance(node, dict):
            for i, log in enumerate(node.get("logs", []) or []):
                all_logs.append((path, i, base64.b64decode(log)))
            for j, inner in enumerate(node.get("inner-txns", []) or []):
                collect_logs(inner, path + f"/inner[{j}]")
    collect_logs(confirmation)
    print(f"\nAll logs ({len(all_logs)}):")
    for path, i, raw in all_logs:
        print(f"  {path}/log[{i}]: prefix={raw[:4].hex()} "
              f"body={raw[4:].hex()[:80]}{'...' if len(raw[4:])>40 else ''}")

    # Find the deepest (chunk-emitted) ABI return log — that's the
    # actual computed result of the user's intended method.
    abi_logs = [(p, i, r) for p, i, r in all_logs
                if r[:4] == bytes.fromhex("151f7c75")]
    assert abi_logs, f"no ABI return log; all_logs={all_logs}"
    # Pick the deepest path (most slashes).
    abi_logs.sort(key=lambda t: t[0].count("/"), reverse=True)
    path, idx, raw = abi_logs[0]
    count = int.from_bytes(raw[4:], "big")
    assert count == 0, (
        f"expected getRoleCount()=0 on fresh deploy, got {count}; "
        f"selected log at {path}/log[{idx}]"
    )
