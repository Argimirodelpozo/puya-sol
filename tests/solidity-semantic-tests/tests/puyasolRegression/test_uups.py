"""UUPS (EIP-1822) lowering (proxy.md §3).

OZ UUPSUpgradeable context checks fold to pass; upgradeToAndCall traps; the
user's `_authorizeUpgrade` (modifiers included) becomes the permission check
of a synthesized UpdateApplication-only gate (`__uups_update`) that emits
ARC-28 Upgraded(address).
"""

import base64

import pytest
from algosdk import account as algosdk_account, transaction
from algosdk.abi import Method

from framework import as_int

REJECTED = "rejected by ApprovalProgram|logic eval error"


def _funded_account(harness, microalgos=1_000_000):
    client = harness.localnet.algod
    acct = harness.localnet.account
    sk, addr = algosdk_account.generate_account()
    params = client.suggested_params()
    pay = transaction.PaymentTxn(acct.address, params, addr, microalgos)
    txid = client.send_transaction(pay.sign(acct.private_key))
    transaction.wait_for_confirmation(client, txid, 4)
    return addr, sk


def _update_app(harness, app, artifacts, name, sender_addr, sender_sk,
                bare=False):
    """Native UpdateApplication through the synthesized __uups_update gate."""
    client = harness.localnet.algod
    entry = artifacts.by_contract[name]
    approval = base64.b64decode(
        client.compile(entry["approval_teal"].read_text())["result"])
    clear = base64.b64decode(
        client.compile(entry["clear_teal"].read_text())["result"])
    params = client.suggested_params()
    selector = Method.from_signature("__uups_update()void").get_selector()
    txn = transaction.ApplicationUpdateTxn(
        sender_addr, params, app.app_id, approval, clear,
        app_args=None if bare else [selector])
    txid = client.send_transaction(txn.sign(sender_sk))
    return transaction.wait_for_confirmation(client, txid, 4)


def test_uups_native_update_gate(harness):
    """puyasolRegression/contracts/uups_impl.sol — flattened OZ v5 UUPS."""
    artifacts = harness.compile("puyasolRegression/contracts/uups_impl.sol")
    app = harness.deploy(artifacts, "UupsBox")
    owner = harness.localnet.account

    # Business logic works; the folded onlyProxy/notDelegated checks pass.
    harness.call(app, "setValue(uint256)", 777)
    assert as_int(harness.call(app, "value()").abi_return) == 777
    # proxiableUUID (notDelegated) returns the ERC-1822 slot constant.
    uuid = bytes(harness.call(app, "proxiableUUID()").abi_return)
    assert uuid.hex() == (
        "360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc")

    # The in-contract upgrade path traps: upgrades are the native ceremony.
    r = harness.call(app, "upgradeToAndCall(address,bytes)",
                     owner.address, b"", expect_revert=True)
    assert r.reverted

    # A stranger cannot update (the gate runs _authorizeUpgrade's onlyOwner).
    stranger_addr, stranger_sk = _funded_account(harness)
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "UupsBox",
                    stranger_addr, stranger_sk)

    # A bare (selector-less) update stays closed.
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "UupsBox",
                    owner.address, owner.private_key, bare=True)

    # The owner updates natively; state survives the program swap.
    _update_app(harness, app, artifacts, "UupsBox",
                owner.address, owner.private_key)
    assert as_int(harness.call(app, "value()").abi_return) == 777
    harness.call(app, "setValue(uint256)", 888)
    assert as_int(harness.call(app, "value()").abi_return) == 888
