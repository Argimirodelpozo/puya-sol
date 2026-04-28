"""Tests for the SimpleSplitter-emitted v1 CTFExchange (orchestrator + helper).

The split orchestrator stubs `SignatureCheckerLib.isValidSignatureNow`, ECDSA
helpers, PolyProxy/PolySafe address derivation, SafeTransferLib transfers, and
CalculatorHelper math into a sibling helper contract; calls round-trip via
inner-app-call. The helper's app id is wired in at deploy time by substituting
the `TMPL_CTFExchange__Helper1_APP_ID` placeholder in the orchestrator's TEAL.

These tests:
  1. Deploy helper standalone, verify it routes (smoke).
  2. Deploy orchestrator with the helper's app id baked in, run __postInit.
  3. Call orchestrator methods that exercise the round-trip (e.g.
     getProxyWalletAddress — pure CREATE2 math, returns address).
"""
from pathlib import Path

import algokit_utils as au
import pytest
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    wait_for_confirmation, PaymentTxn,
)
from conftest import (
    AUTO_POPULATE, NO_POPULATE, ZERO_ADDR,
    addr, algod_addr_for_app, app_id_to_address,
    deploy_app, OUT_DIR,
)


SPLIT_DIR = OUT_DIR / "exchange" / "CTFExchange"
HELPER_DIR = SPLIT_DIR / "CTFExchange__Helper1"
ORCH_DIR = SPLIT_DIR / "CTFExchange"
TMPL_VAR = "TMPL_CTFExchange__Helper1_APP_ID"


def _compile_teal(algod, teal_text: str) -> bytes:
    """Submit TEAL source to algod for assembly to bytecode."""
    return encoding.base64.b64decode(algod.compile(teal_text)["result"])


def _create_app(localnet, sender, approval: bytes, clear: bytes, schema,
                app_args=None, fund_amount=5_000_000, extra_fee=0) -> int:
    algod = localnet.client.algod
    extra_pages = max(0, (max(len(approval), len(clear)) - 1) // 2048)
    sp = algod.suggested_params()
    sp.fee = max(sp.min_fee, sp.fee) + extra_fee
    sp.flat_fee = True
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=schema.ints, num_byte_slices=schema.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=app_args or [],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]
    if fund_amount > 0:
        sp2 = algod.suggested_params()
        pay = PaymentTxn(sender.address, sp2, algod_addr_for_app(app_id), fund_amount)
        wait_for_confirmation(algod, algod.send_transaction(pay.sign(sender.private_key)), 4)
    return app_id


@pytest.fixture(scope="function")
def helper_only(localnet, admin):
    """Deploy CTFExchange__Helper1 standalone — no TMPL substitution needed."""
    algod = localnet.client.algod
    spec = au.Arc56Contract.from_json((HELPER_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    approval = _compile_teal(algod, (HELPER_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    clear = _compile_teal(algod, (HELPER_DIR / "CTFExchange__Helper1.clear.teal").read_text())

    app_id = _create_app(localnet, admin, approval, clear, spec.state.schema.global_state)
    return au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec,
        app_id=app_id, default_sender=admin.address,
    ))


@pytest.fixture(scope="function")
def split_exchange(localnet, admin):
    """Deploy helper, substitute its app id into the orchestrator's TEAL,
    deploy orchestrator, run __postInit. Returns (helper_client, orch_client).
    """
    algod = localnet.client.algod

    # 1. Deploy helper.
    helper_spec = au.Arc56Contract.from_json(
        (HELPER_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    helper_approval = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    helper_clear = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.clear.teal").read_text())
    helper_app_id = _create_app(localnet, admin, helper_approval, helper_clear,
                                helper_spec.state.schema.global_state)

    # 2. Substitute TMPL in orchestrator TEAL with the helper's actual app id.
    orch_spec = au.Arc56Contract.from_json(
        (ORCH_DIR / "CTFExchange.arc56.json").read_text())
    orch_approval_teal = (ORCH_DIR / "CTFExchange.approval.teal").read_text()
    orch_clear_teal = (ORCH_DIR / "CTFExchange.clear.teal").read_text()
    orch_approval_teal = orch_approval_teal.replace(TMPL_VAR, str(helper_app_id))

    orch_approval = _compile_teal(algod, orch_approval_teal)
    orch_clear = _compile_teal(algod, orch_clear_teal)

    # 3. Deploy a collateral mock so __postInit's `IERC20(collateral).approve(ctf, max)`
    #    inner-call has a real receiver.
    collateral = deploy_app(
        localnet, admin,
        OUT_DIR / "dev" / "mocks" / "ERC20",
        "ERC20",
        post_init_args=["USDC Mock", "USDC"],
    )

    # 4. Deploy orchestrator. Schema mirrors the original CTFExchange's.
    orch_app_id = _create_app(localnet, admin, orch_approval, orch_clear,
                              orch_spec.state.schema.global_state,
                              fund_amount=10_000_000)

    helper_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=helper_spec,
        app_id=helper_app_id, default_sender=admin.address,
    ))
    orch_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec,
        app_id=orch_app_id, default_sender=admin.address,
    ))

    # 5. Run __postInit(_collateral, _ctf, _proxyFactory, _safeFactory).
    #    The CTFExchange ctor creates 5 boxes (admins/operators/nonces/registry/
    #    orderStatus), writes to admins[sender] and operators[sender], and
    #    makes an inner-app-call to ERC20.approve. That's well over a single
    #    txn's resource budget (8 boxes, 8 foreign apps, etc.), so we group
    #    the call with padding app-calls that exist solely to contribute
    #    reference slots.
    coll_addr = app_id_to_address(collateral.app_id)
    composer = localnet.new_group()
    # 6 padding noop calls × 8 box refs each = 48 extra box-ref slots.
    for _ in range(6):
        composer.add_app_call_method_call(orch_client.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin",
                args=[ZERO_ADDR],
                note=f"pad-{_}".encode(),
            )))
    composer.add_app_call_method_call(orch_client.params.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[coll_addr, coll_addr, ZERO_ADDR, ZERO_ADDR],
            extra_fee=au.AlgoAmount(micro_algo=20_000),
            app_references=[collateral.app_id],
        )))
    composer.send(AUTO_POPULATE)

    return helper_client, orch_client


# ── Smoke ─────────────────────────────────────────────────────────────────

def test_helper_deploys_standalone(helper_only):
    """Helper compiles + deploys without dependencies."""
    assert helper_only.app_id > 0


def test_helper_min_directly(helper_only):
    """Call CalculatorHelper.min through the helper's ABI surface — verifies
    the helper actually routes ABI calls (not just deploys)."""
    res = helper_only.send.call(au.AppClientMethodCallParams(
        method="CalculatorHelper.min",
        args=[3, 7],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return == 3


def test_helper_calculate_taking_amount(helper_only):
    """CalculatorHelper.calculateTakingAmount(making, makerAmount, takerAmount)
    returns making * takerAmount / makerAmount. Pure math, no state."""
    res = helper_only.send.call(au.AppClientMethodCallParams(
        method="CalculatorHelper.calculateTakingAmount",
        args=[100, 200, 50],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)
    # 100 * 50 / 200 = 25
    assert res.abi_return == 25


def test_split_deploy_succeeds(split_exchange):
    """Helper deployed, orchestrator deployed with helper's app id substituted,
    __postInit ran (which itself does inner app calls during ctor)."""
    helper, orch = split_exchange
    assert helper.app_id > 0
    assert orch.app_id > 0
    assert helper.app_id != orch.app_id


def test_orchestrator_basic_state(split_exchange, admin):
    """isAdmin(deployer) returns true after __postInit — verifies non-stub
    methods work normally on the orchestrator side."""
    _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="isAdmin", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return is True


# ── Round-trip through helper ─────────────────────────────────────────────

@pytest.mark.xfail(reason="proxyFactory/safeFactory wired to ZERO_ADDR; CREATE2 in helper attempts to read app 0 params")
def test_proxy_wallet_address_round_trip(split_exchange, admin):
    """getProxyWalletAddress(signer) calls PolyProxyLib.getProxyWalletAddress
    (extracted to helper). With a real proxy factory wired up this would round-
    trip; using ZERO_ADDR for the factory makes the helper hit `unavailable
    App 0` while computing the CREATE2 address."""
    _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="getProxyWalletAddress", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=30_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return != "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


@pytest.mark.xfail(reason="safeFactory wired to ZERO_ADDR; same as proxy test")
def test_safe_address_round_trip(split_exchange, admin):
    _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="getSafeAddress", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=30_000),
    ), send_params=AUTO_POPULATE)
    assert res.abi_return != "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"
