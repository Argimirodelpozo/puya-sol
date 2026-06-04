"""ARC-20 (Smart ASA) conformance tests for MyArc20 (WIP/tokens/ARC20.sol).

Task 54a: asset_create + the four readonly getters (get_asset_config,
get_circulating_supply, get_asset_is_frozen, get_account_is_frozen).

MyArc20 is an ARC-20 Smart ASA compiled to the AVM by puya-sol. The app creates
and controls a single underlying ASA (app = all ASA roles); freeze/config are
app-level state. Deploy is create -> fund -> __postInit, then asset_create fires
the inner acfg that births the controlled ASA.
"""

import base64
import json
import math

import algokit_utils as au
import pytest
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
)
from conftest import (  # type: ignore[import-not-found]
    OUT_DIR,
    app_address,
    fund_account,
    load_arc56,
    opt_in_to_asa,
)

FEE2 = au.AlgoAmount.from_micro_algo(2_000)  # one inner asaTransfer


def _asa_bal(localnet, addr: str, asa: int) -> int:
    return localnet.client.algod.account_asset_info(addr, asa)["asset-holding"]["amount"]


def _xfer(client, asa, amount, sender, receiver):
    client.send.call(
        au.AppClientMethodCallParams(
            method="asset_transfer", args=[asa, amount, sender, receiver], extra_fee=FEE2),
        send_params={"populate_app_call_resources": True})


def _setup_holder(localnet, account):
    """Deploy + asset_create (account = all admin roles), opt account + a second
    holder in, return (admin_client, app_id, asa, acct2, acct2_client). acct2 is a
    NON-admin holder, so its transfers hit the regular (non-clawback) branch."""
    client, app_id = deploy_arc20(localnet, account)
    asa = _create(client, account)
    opt_in_to_asa(localnet, account, asa)
    acct2 = localnet.account.random()
    fund_account(localnet, account, acct2.address, 1_000_000)
    opt_in_to_asa(localnet, acct2, asa)
    acct2_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=load_arc56("MyArc20"), app_id=app_id,
        default_sender=acct2.address))
    return client, app_id, asa, acct2, acct2_client

TOTAL = 1_000_000
DECIMALS = 6
UNIT = "UARC"
NAME = "My ARC20"
URL = "https://arc20.example"


def deploy_arc20(localnet, account, name: str = "MyArc20", fund: int = 2_000_000):
    """Create -> fund -> __postInit. Returns (AppClient, app_id)."""
    algod = localnet.client.algod
    spec = load_arc56(name)
    ap = base64.b64decode(algod.compile((OUT_DIR / f"{name}.approval.teal").read_text())["result"])
    cl = base64.b64decode(algod.compile((OUT_DIR / f"{name}.clear.teal").read_text())["result"])
    sch = json.loads((OUT_DIR / f"{name}.arc56.json").read_text())["state"]["schema"]
    txn = ApplicationCreateTxn(
        account.address, algod.suggested_params(), OnComplete.NoOpOC, ap, cl,
        StateSchema(sch["global"]["ints"], sch["global"]["bytes"]),
        StateSchema(sch["local"]["ints"], sch["local"]["bytes"]),
        extra_pages=math.ceil(len(ap) / 2048) - 1,
    )
    app_id = wait_for_confirmation(
        algod, algod.send_transaction(txn.sign(account.private_key)), 4
    )["application-index"]
    fund_account(localnet, account, app_address(app_id), fund)
    client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=app_id, default_sender=account.address))
    client.send.call(au.AppClientMethodCallParams(
        method="__postInit", args=[], extra_fee=au.AlgoAmount.from_micro_algo(2_000)))
    return client, app_id


def _create(client, account):
    """asset_create with the account as all four admin roles. Returns the ASA id."""
    return int(client.send.call(au.AppClientMethodCallParams(
        method="asset_create",
        args=[TOTAL, DECIMALS, False, UNIT, NAME, URL, b"",
              account.address, account.address, account.address, account.address],
        extra_fee=au.AlgoAmount.from_micro_algo(3_000),
    )).abi_return)


def test_asset_create_and_get_config(localnet, account):
    """asset_create births the ASA and stores config readable via get_asset_config."""
    client, _ = deploy_arc20(localnet, account)
    asa = _create(client, account)
    assert asa > 0
    assert int(client.send.call(au.AppClientMethodCallParams(method="smartAsaId")).abi_return) == asa

    cfg = client.send.call(
        au.AppClientMethodCallParams(method="get_asset_config", args=[asa])).abi_return
    # AssetConfig struct -> decoded as a dict keyed by the Solidity field names.
    assert cfg["total"] == TOTAL
    assert cfg["decimals"] == DECIMALS
    assert cfg["defaultFrozen"] is False
    assert cfg["unitName"] == UNIT
    assert cfg["name"] == NAME
    assert cfg["url"] == URL
    assert cfg["managerAddr"] == account.address
    assert cfg["clawbackAddr"] == account.address


def test_circulating_supply_zero_at_create(localnet, account):
    """At create the app (creator) holds every unit, so circulating supply is 0."""
    client, _ = deploy_arc20(localnet, account)
    asa = _create(client, account)
    circ = int(client.send.call(
        au.AppClientMethodCallParams(method="get_circulating_supply", args=[asa])).abi_return)
    assert circ == 0


def test_frozen_flags_default_false(localnet, account):
    """Both freeze flags read false on a fresh asset."""
    client, _ = deploy_arc20(localnet, account)
    asa = _create(client, account)
    assert client.send.call(au.AppClientMethodCallParams(
        method="get_asset_is_frozen", args=[asa])).abi_return is False
    assert client.send.call(au.AppClientMethodCallParams(
        method="get_account_is_frozen", args=[asa, account.address])).abi_return is False


# ── 54b: asset_transfer (mint / regular / clawback) + asset_freeze + account_freeze ──

def test_mint_and_regular_transfer(localnet, account):
    """Mint (asset_sender == app, reserve-authorized) then a regular holder transfer."""
    client, app_id, asa, acct2, acct2_client = _setup_holder(localnet, account)
    app_addr = app_address(app_id)
    _xfer(client, asa, 1000, app_addr, acct2.address)  # mint to acct2
    assert _asa_bal(localnet, acct2.address, asa) == 1000
    assert int(client.send.call(au.AppClientMethodCallParams(
        method="get_circulating_supply", args=[asa])).abi_return) == 1000
    # regular: acct2 -> account (caller is acct2, not the clawback admin)
    _xfer(acct2_client, asa, 400, acct2.address, account.address)
    assert _asa_bal(localnet, acct2.address, asa) == 600
    assert _asa_bal(localnet, account.address, asa) == 400


def test_account_freeze_blocks_then_clawback_bypasses(localnet, account):
    """A frozen account can't move tokens, but the clawback admin still can."""
    client, app_id, asa, acct2, acct2_client = _setup_holder(localnet, account)
    _xfer(client, asa, 1000, app_address(app_id), acct2.address)  # mint to acct2
    client.send.call(au.AppClientMethodCallParams(
        method="account_freeze", args=[asa, acct2.address, True]),
        send_params={"populate_app_call_resources": True})
    assert client.send.call(au.AppClientMethodCallParams(
        method="get_account_is_frozen", args=[asa, acct2.address]),
        send_params={"populate_app_call_resources": True}).abi_return is True
    with pytest.raises(Exception):  # frozen holder cannot transfer
        _xfer(acct2_client, asa, 100, acct2.address, account.address)
    # clawback admin (account == clawbackAddr) bypasses the freeze
    _xfer(client, asa, 300, acct2.address, account.address)
    assert _asa_bal(localnet, acct2.address, asa) == 700
    assert _asa_bal(localnet, account.address, asa) == 300


def test_asset_freeze_blocks_and_unfreeze_restores(localnet, account):
    """Global freeze halts regular transfers; unfreeze restores them."""
    client, app_id, asa, acct2, acct2_client = _setup_holder(localnet, account)
    _xfer(client, asa, 1000, app_address(app_id), acct2.address)  # mint to acct2
    client.send.call(au.AppClientMethodCallParams(method="asset_freeze", args=[asa, True]))
    assert client.send.call(au.AppClientMethodCallParams(
        method="get_asset_is_frozen", args=[asa])).abi_return is True
    with pytest.raises(Exception):
        _xfer(acct2_client, asa, 100, acct2.address, account.address)
    client.send.call(au.AppClientMethodCallParams(method="asset_freeze", args=[asa, False]))
    _xfer(acct2_client, asa, 100, acct2.address, account.address)
    assert _asa_bal(localnet, acct2.address, asa) == 900
