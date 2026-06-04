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

import algokit_utils as au
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
)

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
