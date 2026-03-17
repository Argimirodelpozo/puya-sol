from pathlib import Path
import json
import base64

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    ApplicationCallTxn, PaymentTxn, wait_for_confirmation,
    assign_group_id,
)
from algosdk import abi, encoding
from algokit_utils.models.account import SigningAccount
import pytest

OUT_DIR = Path(__file__).parent.parent / "out" / "UD60x18Wrapper"

# Number of extra NoOp calls for opcode budget pooling (each adds 700 budget)
BUDGET_PADDING = 14  # 14 * 700 = 9800 extra budget

# AVM limit: max 8 foreign apps per transaction
MAX_FOREIGN_APPS = 8


@pytest.fixture(scope="session")
def algod_client() -> AlgodClient:
    config = au.ClientManager.get_default_localnet_config("algod")
    return au.ClientManager.get_algod_client(config)


@pytest.fixture(scope="session")
def kmd_client() -> KMDClient:
    config = au.ClientManager.get_default_localnet_config("kmd")
    return au.ClientManager.get_kmd_client(config)


@pytest.fixture(scope="session")
def localnet_clients(algod_client, kmd_client):
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


@pytest.fixture(scope="session")
def account(localnet_clients):
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


@pytest.fixture(scope="session")
def algod(algod_client):
    return algod_client


def _deploy_app(algod_client, account, name, extra_pages=0):
    """Deploy a single app and return its app_id."""
    approval = (OUT_DIR / f"{name}.approval.teal").read_text()
    clear = (OUT_DIR / f"{name}.clear.teal").read_text()
    approval_compiled = algod_client.compile(approval)["result"]
    clear_compiled = algod_client.compile(clear)["result"]

    sp = algod_client.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=base64.b64decode(approval_compiled),
        clear_program=base64.b64decode(clear_compiled),
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(0, 0),
        extra_pages=extra_pages,
    )
    signed = txn.sign(account.private_key)
    txid = algod_client.send_transaction(signed)
    result = wait_for_confirmation(algod_client, txid, 4)
    return result["application-index"]


def _fund_app(algod_client, account, app_id, amount=10_000_000):
    """Fund an app account."""
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    sp = algod_client.suggested_params()
    pay_txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed_pay = pay_txn.sign(account.private_key)
    wait_for_confirmation(algod_client, algod_client.send_transaction(signed_pay), 4)


@pytest.fixture(scope="session")
def helper_ids(algod_client, account):
    """Deploy helper contracts and return their app IDs."""
    helpers = []
    i = 1
    while (OUT_DIR / f"UD60x18Wrapper__Helper{i}.approval.teal").exists():
        aid = _deploy_app(algod_client, account, f"UD60x18Wrapper__Helper{i}", extra_pages=3)
        _fund_app(algod_client, account, aid)
        helpers.append(aid)
        i += 1
    return helpers


@pytest.fixture(scope="session")
def app_id(algod_client, account, helper_ids):
    aid = _deploy_app(algod_client, account, "UD60x18Wrapper", extra_pages=3)
    _fund_app(algod_client, account, aid)
    return aid


@pytest.fixture(scope="session")
def arc56_spec():
    with open(OUT_DIR / "UD60x18Wrapper.arc56.json") as f:
        return json.load(f)


@pytest.fixture(scope="session")
def helper_specs():
    """Load ARC56 specs for all helpers."""
    specs = []
    i = 1
    while (OUT_DIR / f"UD60x18Wrapper__Helper{i}.arc56.json").exists():
        with open(OUT_DIR / f"UD60x18Wrapper__Helper{i}.arc56.json") as f:
            specs.append(json.load(f))
        i += 1
    return specs


def _make_sp(algod_client, fee=20_000):
    sp = algod_client.suggested_params()
    sp.fee = fee
    sp.flat_fee = True
    return sp


def _limit_foreign_apps(app_ids):
    """Limit foreign apps to AVM maximum of 8."""
    return app_ids[:MAX_FOREIGN_APPS]


@pytest.fixture(scope="session")
def call(algod_client, account, app_id, arc56_spec, helper_ids, helper_specs):
    """Returns a callable: call(method_name, *args) -> decoded return value.

    For local methods: sends call + budget padding in atomic group.
    For delegated methods: sends orchestrator.method → helper.method →
    orchestrator.__finish_method + budget padding in atomic group.
    """
    orch_methods = {m["name"]: abi.Method.undictify(m) for m in arc56_spec["methods"]}

    # Identify delegated methods (those with __finish_* counterparts)
    delegated = set()
    for name in orch_methods:
        if f"__finish_{name}" in orch_methods:
            delegated.add(name)

    # Build helper method lookup: method_name -> (helper_index, abi.Method)
    helper_method_map = {}
    for idx, spec in enumerate(helper_specs):
        for m in spec["methods"]:
            if m["name"] not in ("__init__",):
                helper_method_map[m["name"]] = (idx, abi.Method.undictify(m))

    # Pre-compute __auth__ selector
    auth_selector = None
    for name, m in orch_methods.items():
        if name == "__auth__":
            auth_selector = m.get_selector()
            break

    def _call(method_name, *args):
        method = orch_methods[method_name]

        # Build app_args for this method
        app_args = [method.get_selector()]
        for i, arg in enumerate(args):
            app_args.append(method.args[i].type.encode(arg))

        if method_name not in delegated:
            # Local method: call + budget padding
            main_txn = ApplicationCallTxn(
                sender=account.address,
                sp=_make_sp(algod_client),
                index=app_id,
                on_complete=OnComplete.NoOpOC,
                app_args=app_args,
                foreign_apps=_limit_foreign_apps(helper_ids),
            )

            # Budget padding txns — no foreign apps needed for __auth__
            budget_txns = [
                ApplicationCallTxn(
                    sender=account.address,
                    sp=_make_sp(algod_client, fee=0),
                    index=app_id,
                    on_complete=OnComplete.NoOpOC,
                    app_args=[auth_selector] if auth_selector else [],
                    note=f"b{i}".encode(),
                )
                for i in range(BUDGET_PADDING)
            ]

            group = [main_txn] + budget_txns
            assign_group_id(group)
            signed = [t.sign(account.private_key) for t in group]
            txid = algod_client.send_transactions(signed)
            result = wait_for_confirmation(algod_client, txid, 4)

            if "logs" in result and result["logs"]:
                raw = base64.b64decode(result["logs"][-1])
                if raw[:4] == b"\x15\x1f\x7c\x75":
                    return method.returns.type.decode(raw[4:])
            return None
        else:
            # Delegated method: 3-txn + budget padding
            helper_idx, helper_method = helper_method_map[method_name]
            helper_app_id = helper_ids[helper_idx]
            finish_method = orch_methods[f"__finish_{method_name}"]

            helper_app_args = [helper_method.get_selector()]
            for i, arg in enumerate(args):
                helper_app_args.append(helper_method.args[i].type.encode(arg))

            finish_app_args = [finish_method.get_selector()]

            # Build foreign_apps for orchestrator: helper + up to 7 others
            orch_foreign = [helper_app_id] + [
                h for h in helper_ids if h != helper_app_id
            ]
            orch_foreign = _limit_foreign_apps(orch_foreign)

            # Build foreign_apps for helper: orchestrator + up to 7 others
            helper_foreign = [app_id] + [
                h for h in helper_ids if h != helper_app_id
            ]
            helper_foreign = _limit_foreign_apps(helper_foreign)

            # Txn 1: orchestrator.method(args)
            txn1 = ApplicationCallTxn(
                sender=account.address,
                sp=_make_sp(algod_client),
                index=app_id,
                on_complete=OnComplete.NoOpOC,
                app_args=app_args,
                foreign_apps=orch_foreign,
            )

            # Txn 2: helper.method(args)
            txn2 = ApplicationCallTxn(
                sender=account.address,
                sp=_make_sp(algod_client),
                index=helper_app_id,
                on_complete=OnComplete.NoOpOC,
                app_args=helper_app_args,
                foreign_apps=helper_foreign,
            )

            # Txn 3: orchestrator.__finish_method()
            txn3 = ApplicationCallTxn(
                sender=account.address,
                sp=_make_sp(algod_client),
                index=app_id,
                on_complete=OnComplete.NoOpOC,
                app_args=finish_app_args,
                foreign_apps=orch_foreign,
            )

            # Budget padding — no foreign apps needed for __auth__
            budget_txns = [
                ApplicationCallTxn(
                    sender=account.address,
                    sp=_make_sp(algod_client, fee=0),
                    index=app_id,
                    on_complete=OnComplete.NoOpOC,
                    app_args=[auth_selector] if auth_selector else [],
                    note=f"b{i}".encode(),
                )
                for i in range(min(BUDGET_PADDING, 13))  # max 16 txns per group
            ]

            group = [txn1, txn2, txn3] + budget_txns
            assign_group_id(group)
            signed = [t.sign(account.private_key) for t in group]
            txid = algod_client.send_transactions(signed)
            result = wait_for_confirmation(algod_client, txid, 4)

            # __finish result comes from txn3
            finish_txid = group[2].get_txid()
            finish_result = wait_for_confirmation(algod_client, finish_txid, 4)

            if "logs" in finish_result and finish_result["logs"]:
                raw = base64.b64decode(finish_result["logs"][-1])
                if raw[:4] == b"\x15\x1f\x7c\x75":
                    return method.returns.type.decode(raw[4:])
            return None

    return _call


# UD60x18 constants
UNIT = 10**18
HALF_UNIT = 5 * 10**17
MAX_UD60x18 = 2**256 - 1
MAX_WHOLE_UD60x18 = MAX_UD60x18 - (MAX_UD60x18 % UNIT)
