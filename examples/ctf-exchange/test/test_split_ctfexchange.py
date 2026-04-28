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


from conftest import HELPER_DIR, _compile_teal  # shared helpers
SPLIT_DIR = OUT_DIR / "exchange" / "CTFExchange"
ORCH_DIR = SPLIT_DIR / "CTFExchange"
TMPL_VAR = "TMPL_CTFExchange__Helper1_APP_ID"


# split_exchange fixture lives in conftest.py


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

@pytest.mark.xfail(reason="puya-sol mistranslates `mstore(add(buffer, 0x20), "
                          "<32-byte literal>)` for `bytes memory buffer = new "
                          "bytes(N)` allocations — the EVM scratch-write "
                          "pattern doesn't reach the simulated memory blob; "
                          "PolyProxyLib._computeCreationCode trips at runtime")
def test_proxy_wallet_address_round_trip(split_exchange, admin):
    """getPolyProxyWalletAddress(signer) calls PolyProxyLib.getProxyWalletAddress
    (extracted to helper). With ZERO_ADDR for proxyFactory/proxyImplementation
    in __postInit, the CREATE2 result is deterministic but reflects the
    zero-addressed inputs — we just verify the call succeeds and returns
    32 bytes."""
    _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="getPolyProxyWalletAddress", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=30_000),
    ), send_params=AUTO_POPULATE)
    # Result is a 58-char base32 algo address string from algokit's decoder.
    assert isinstance(res.abi_return, str)
    assert len(res.abi_return) == 58


@pytest.mark.xfail(reason="same `mstore(add(buf, 0x20), literal)` mistranslation "
                          "as proxy round-trip — PolySafeLib.getContractBytecode")
def test_safe_address_round_trip(split_exchange, admin):
    _, orch = split_exchange
    res = orch.send.call(au.AppClientMethodCallParams(
        method="getSafeAddress", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=30_000),
    ), send_params=AUTO_POPULATE)
    assert isinstance(res.abi_return, str)
    assert len(res.abi_return) == 58
