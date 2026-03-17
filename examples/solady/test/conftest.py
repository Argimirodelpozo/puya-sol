"""Shared fixtures for Solady contract tests on Algorand."""
from pathlib import Path
import json
import base64

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    ApplicationCallTxn, PaymentTxn, wait_for_confirmation,
)
from algosdk import abi, encoding
from algokit_utils.models.account import SigningAccount
import pytest

OUT_DIR = Path(__file__).parent.parent / "out"


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


def deploy_contract(algod_client, account, name, extra_pages=0, fund_amount=1_000_000):
    """Deploy a compiled Solady contract and return (app_id, arc56_spec)."""
    contract_dir = OUT_DIR / name
    approval = (contract_dir / f"{name}.approval.teal").read_text()
    clear = (contract_dir / f"{name}.clear.teal").read_text()
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
    app_id = result["application-index"]

    # Fund the contract
    if fund_amount > 0:
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
        )
        sp = algod_client.suggested_params()
        pay_txn = PaymentTxn(account.address, sp, app_addr, fund_amount)
        signed_pay = pay_txn.sign(account.private_key)
        wait_for_confirmation(algod_client, algod_client.send_transaction(signed_pay), 4)

    with open(contract_dir / f"{name}.arc56.json") as f:
        arc56_spec = json.load(f)

    return app_id, arc56_spec


def deploy_split_contract(algod_client, account, name, num_helpers, extra_pages=0, fund_amount=1_000_000):
    """Deploy a split contract (orchestrator + helpers) and return (app_id, arc56_spec).

    The orchestrator needs the helper app IDs passed as app references in calls.
    """
    helper_ids = []
    for i in range(1, num_helpers + 1):
        helper_name = f"{name}__Helper{i}"
        helper_dir = OUT_DIR / name
        h_approval = (helper_dir / f"{helper_name}.approval.teal").read_text()
        h_clear = (helper_dir / f"{helper_name}.clear.teal").read_text()
        h_approval_compiled = algod_client.compile(h_approval)["result"]
        h_clear_compiled = algod_client.compile(h_clear)["result"]

        sp = algod_client.suggested_params()
        txn = ApplicationCreateTxn(
            sender=account.address,
            sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=base64.b64decode(h_approval_compiled),
            clear_program=base64.b64decode(h_clear_compiled),
            global_schema=StateSchema(0, 0),
            local_schema=StateSchema(0, 0),
            extra_pages=extra_pages,
        )
        signed = txn.sign(account.private_key)
        result = wait_for_confirmation(algod_client, algod_client.send_transaction(signed), 4)
        helper_id = result["application-index"]
        helper_ids.append(helper_id)

        # Fund helper
        if fund_amount > 0:
            app_addr = encoding.encode_address(
                encoding.checksum(b"appID" + helper_id.to_bytes(8, "big"))
            )
            sp = algod_client.suggested_params()
            pay_txn = PaymentTxn(account.address, sp, app_addr, fund_amount)
            signed_pay = pay_txn.sign(account.private_key)
            wait_for_confirmation(algod_client, algod_client.send_transaction(signed_pay), 4)

    # Deploy orchestrator
    app_id, arc56_spec = deploy_contract(
        algod_client, account, name, extra_pages=extra_pages, fund_amount=fund_amount
    )

    return app_id, arc56_spec, helper_ids


def make_caller(algod_client, account, app_id, arc56_spec, foreign_apps=None):
    """Create a callable: call(method_name, *args) -> decoded return value."""
    methods = {m["name"]: abi.Method.undictify(m) for m in arc56_spec["methods"]}

    def _call(method_name, *args):
        method = methods[method_name]
        sp = algod_client.suggested_params()
        sp.fee = 10_000
        sp.flat_fee = True

        app_args = [method.get_selector()]
        for i, arg in enumerate(args):
            app_args.append(method.args[i].type.encode(arg))

        txn = ApplicationCallTxn(
            sender=account.address,
            sp=sp,
            index=app_id,
            on_complete=OnComplete.NoOpOC,
            app_args=app_args,
            foreign_apps=foreign_apps or [],
        )
        signed = txn.sign(account.private_key)
        txid = algod_client.send_transaction(signed)
        result = wait_for_confirmation(algod_client, txid, 4)

        if "logs" in result and result["logs"]:
            raw = base64.b64decode(result["logs"][-1])
            if raw[:4] == b"\x15\x1f\x7c\x75":
                return_type = method.returns.type
                if return_type is not None:
                    return return_type.decode(raw[4:])
        return None

    return _call
