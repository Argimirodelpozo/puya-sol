"""EIP-1967 proxy-slot lowering (proxy.md §1).

Admin slot → synthesized app global + bare UpdateApplication gate;
implementation slot → the app's own identity; upgradeTo → runtime trap.
"""

import base64

import pytest
from algosdk import transaction
from algosdk.encoding import decode_address

from framework import as_int


def _addr_bytes(abi_return):
    """abi_return of an `address` normalized to its 32 raw bytes."""
    if isinstance(abi_return, str):
        return decode_address(abi_return)
    return bytes(abi_return)[-32:]


def _update_app(harness, app, artifacts, name, sender_addr, sender_sk,
                bare=False):
    """Submit a native UpdateApplication reusing the app's own programs.

    The synthesized gate is an ARC-4 ABI method, so the txn carries its
    selector; bare=True omits it to prove selector-less updates stay closed.
    """
    client = harness.localnet.algod
    entry = artifacts.by_contract[name]
    approval = base64.b64decode(
        client.compile(entry["approval_teal"].read_text())["result"])
    clear = base64.b64decode(
        client.compile(entry["clear_teal"].read_text())["result"])
    params = client.suggested_params()
    from algosdk.abi import Method
    selector = Method.from_signature("__erc1967_update()void").get_selector()
    txn = transaction.ApplicationUpdateTxn(
        sender_addr, params, app.app_id, approval, clear,
        app_args=None if bare else [selector])
    signed = txn.sign(sender_sk)
    txid = client.send_transaction(signed)
    return transaction.wait_for_confirmation(client, txid, 4)


def _run_flow(harness, extra_args):
    artifacts = harness.compile(
        "puyasolRegression/contracts/erc1967_impl.sol",
        extra_args=extra_args,
    )
    app = harness.deploy(artifacts, "Erc1967Impl")
    acct = harness.localnet.account

    # Unset admin slot reads zero (EVM unset-slot semantics).
    assert as_int(harness.call(app, "admin()").abi_return) == 0

    # Implementation slot = this app's own identity (bytes24 ++ app id).
    assert as_int(harness.call(app, "implementation()").abi_return) == app.app_id

    # Fail closed: with a zero admin, native updates are rejected.
    with pytest.raises(Exception):
        _update_app(harness, app, artifacts, "Erc1967Impl", acct.address, acct.private_key)

    # Admin round-trips through the synthesized global.
    harness.call(app, "initAdmin(address)", acct.address)
    got = harness.call(app, "admin()").abi_return
    assert _addr_bytes(got) == decode_address(acct.address)

    # upgradeTo is a runtime trap with the native-ceremony message.
    r = harness.call(app, "upgradeTo(address)", acct.address, expect_revert=True)
    assert r.reverted

    # The native update ceremony works for the admin — and preserves state.
    harness.call(app, "setValue(uint256)", 777)
    info = _update_app(
        harness, app, artifacts, "Erc1967Impl", acct.address, acct.private_key)
    # The gate emits ARC-28 Upgraded(address) — EIP-1967's event signature —
    # inside the UpdateApplication txn; the "implementation" is the app's own
    # identity (bytes24 ++ app id).
    import base64
    import hashlib
    want = (hashlib.new("sha512_256", b"Upgraded(address)").digest()[:4]
            + bytes(24) + app.app_id.to_bytes(8, "big"))
    logs = [base64.b64decode(l) for l in info.get("logs") or []]
    assert want in logs, [l.hex() for l in logs]
    assert as_int(harness.call(app, "value()").abi_return) == 777

    # A selector-less (bare) update is rejected even for the admin.
    with pytest.raises(Exception):
        _update_app(harness, app, artifacts, "Erc1967Impl",
                    acct.address, acct.private_key, bare=True)
    got = harness.call(app, "admin()").abi_return
    assert _addr_bytes(got) == decode_address(acct.address)


def test_erc1967_default_mode(harness):
    _run_flow(harness, None)


def test_erc1967_evm_layout(harness):
    _run_flow(harness, ["--evm-layout"])
