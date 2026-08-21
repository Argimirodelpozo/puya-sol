"""EIP-1967 proxy-slot lowering (proxy.md §1).

Admin slot → synthesized app global + bare UpdateApplication gate;
implementation slot → the app's own identity; upgradeTo → runtime trap.
"""

import base64

import pytest
from algosdk import account as algosdk_account, transaction
from algosdk.encoding import decode_address

from framework import as_int

# Specific rejection only — a broad Exception match would also pass on the
# known algod-30s-timeout flake class.
REJECTED = "rejected by ApprovalProgram|logic eval error"


def _funded_account(harness, microalgos=1_000_000):
    """Generate a fresh account and fund it from the dispenser account."""
    client = harness.localnet.algod
    acct = harness.localnet.account
    sk, addr = algosdk_account.generate_account()
    params = client.suggested_params()
    pay = transaction.PaymentTxn(acct.address, params, addr, microalgos)
    txid = client.send_transaction(pay.sign(acct.private_key))
    transaction.wait_for_confirmation(client, txid, 4)
    return addr, sk


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

    # Proxy-mode deployment defers implementation storage initialization.
    # Both a typed read and the shared sload dispatcher must therefore map an
    # absent AVM global to Solidity's zero default, not assert key existence.
    assert as_int(harness.call(app, "value()").abi_return) == 0
    assert as_int(harness.call(app, "rawValue()").abi_return) == 0

    # Implementation slot = this app's own identity (bytes24 ++ app id).
    assert as_int(harness.call(app, "implementation()").abi_return) == app.app_id

    # Fail closed: with a zero admin, native updates are rejected.
    with pytest.raises(Exception, match=REJECTED):
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
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "Erc1967Impl",
                    acct.address, acct.private_key, bare=True)
    got = harness.call(app, "admin()").abi_return
    assert _addr_bytes(got) == decode_address(acct.address)

    # Beacon slot has no AVM analogue: both directions trap at runtime.
    assert harness.call(app, "beacon()", expect_revert=True).reverted
    assert harness.call(
        app, "setBeacon(address)", acct.address, expect_revert=True).reverted

    # changeAdmin hands the gate to a second account: the old admin is
    # rejected, the new admin's native update succeeds.
    new_addr, new_sk = _funded_account(harness)
    harness.call(app, "changeAdmin(address)", new_addr)
    assert _addr_bytes(harness.call(app, "admin()").abi_return) \
        == decode_address(new_addr)
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "Erc1967Impl",
                    acct.address, acct.private_key)
    _update_app(harness, app, artifacts, "Erc1967Impl", new_addr, new_sk)
    assert as_int(harness.call(app, "value()").abi_return) == 777


def test_erc1967_default_mode(harness):
    _run_flow(harness, None)


def test_erc1967_evm_layout(harness):
    _run_flow(harness, ["--evm-layout"])


# Admin-slot assembly reached through a LIBRARY (the OZ
# ERC1967Utils shape) must arm the gate on the contract whose call graph
# reaches it. The pre-fix unit-global flag was consumed by the FIRST contract
# built: Unrelated grew the admin global/gate and LibProxy got neither (its
# admin writes then blew the app's declared schema).

def _run_lib_flow(harness, extra_args):
    artifacts = harness.compile(
        "puyasolRegression/contracts/erc1967_lib_multi.sol",
        extra_args=extra_args,
    )
    acct = harness.localnet.account

    # Unrelated (first in the unit, 1967-free) works and carries NO gate.
    unrelated = harness.deploy(artifacts, "Unrelated")
    harness.call(unrelated, "setX(uint256)", 5)
    assert as_int(harness.call(unrelated, "x()").abi_return) == 5
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, unrelated, artifacts, "Unrelated",
                    acct.address, acct.private_key)

    # LibProxy: the gate attaches via the library call graph.
    proxy = harness.deploy(artifacts, "LibProxy")
    assert as_int(harness.call(proxy, "admin()").abi_return) == 0
    harness.call(proxy, "initAdmin(address)", acct.address)
    got = harness.call(proxy, "admin()").abi_return
    assert _addr_bytes(got) == decode_address(acct.address)

    harness.call(proxy, "setValue(uint256)", 777)
    _update_app(harness, proxy, artifacts, "LibProxy",
                acct.address, acct.private_key)
    assert as_int(harness.call(proxy, "value()").abi_return) == 777


def test_erc1967_library_attribution_default(harness):
    _run_lib_flow(harness, None)


def test_erc1967_library_attribution_evm_layout(harness):
    _run_lib_flow(harness, ["--evm-layout"])


# ── 1967 slot-constant data flow + admin forms (erc1967_slot_flow.sol) ──────

def test_erc1967_let_bound_slot(harness):
    """`let s := _ADMIN_SLOT; sstore(s, v)` — the OZ ERC1967Utils body shape.

    The let-bound slot constant is folded so classification fires: the full
    admin round-trip and gated native update work exactly as with a literal
    slot argument.
    """
    artifacts = harness.compile("puyasolRegression/contracts/erc1967_slot_flow.sol")
    app = harness.deploy(artifacts, "LetSlot")
    acct = harness.localnet.account

    assert as_int(harness.call(app, "admin()").abi_return) == 0
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "LetSlot",
                    acct.address, acct.private_key)
    harness.call(app, "initAdmin(address)", acct.address)
    assert _addr_bytes(harness.call(app, "admin()").abi_return) \
        == decode_address(acct.address)
    _update_app(harness, app, artifacts, "LetSlot",
                acct.address, acct.private_key)


def test_erc1967_escaped_slot_warning(harness):
    """A slot constant flowing through a function param (OZ StorageSlot's
    getAddressSlot) cannot be classified — the compiler must WARN that
    storage through the derived slot splits from the native proxy model.
    """
    harness.compile("puyasolRegression/contracts/erc1967_slot_flow.sol")
    log = (harness.out_dir / "puya-sol.log").read_text()
    assert "escapes into a runtime context" in log


def test_erc1967_contract_valued_admin(harness):
    """Admin stored as a CONTRACT — identity form (bytes24 ++ app id, the
    ProxyAdmin topology) or escrow form (`address(this)`): the gate matches
    the identity form against that app's ESCROW address via app_params_get,
    so no EOA can update either way (fail-closed, not open).
    """
    artifacts = harness.compile("puyasolRegression/contracts/erc1967_slot_flow.sol")
    acct = harness.localnet.account

    # Identity form: raw word == bytes24(0) ++ itob(appId).
    app = harness.deploy(artifacts, "SelfAdmin")
    harness.call(app, "initAdminApp(uint256)", app.app_id)
    assert as_int(harness.call(app, "admin()").abi_return) == app.app_id
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "SelfAdmin",
                    acct.address, acct.private_key)
    # Structural pin: the gate maps an identity-form admin via its escrow.
    teal = artifacts.by_contract["SelfAdmin"]["approval_teal"].read_text()
    assert "app_params_get AppAddress" in teal

    # Escrow form: `address(this)` stores the app's escrow ACCOUNT — the
    # direct-compare arm applies, and an EOA still can't satisfy it.
    app2 = harness.deploy(artifacts, "SelfAdmin")
    harness.call(app2, "initAdminSelf()")
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app2, artifacts, "SelfAdmin",
                    acct.address, acct.private_key)


def test_erc1967_impl_only_no_gate(harness):
    """UUPS shape (implementation slot only, no admin use): NO gate is
    synthesized — native updates stay rejected, fail-closed (proxy.md §1).
    """
    artifacts = harness.compile("puyasolRegression/contracts/erc1967_lib_multi.sol")
    app = harness.deploy(artifacts, "ImplOnly")
    acct = harness.localnet.account

    assert as_int(harness.call(app, "implementation()").abi_return) == app.app_id
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "ImplOnly",
                    acct.address, acct.private_key)
    with pytest.raises(Exception, match=REJECTED):
        _update_app(harness, app, artifacts, "ImplOnly",
                    acct.address, acct.private_key, bare=True)
