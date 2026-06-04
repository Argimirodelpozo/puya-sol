"""Pytest fixtures for aerc20-demo tests — localnet client, account, deploy
helpers. Mirrors the OpenZeppelin test scaffold so the same patterns work.

The big difference from OpenZeppelin tests: the AERC20 base contract creates
an ASA from inside __postInit via an inner `acfg` transaction. The deploy
sequence is therefore:

    1. AppCreate (raw ApplicationCreateTxn — sets __ctor_pending=1)
    2. Fund the app's address (~ 1M algo so the inner acfg's 0.1 algo MBR
       + each subsequent inner txn fee is covered by the contract balance)
    3. Call __postInit (with extra fee budget, since one acfg fires inside)

Tests that go on to clawback-transfer tokens around will then need both
parties to opt-in to the ASA before the contract's `transfer` works.
"""

from pathlib import Path

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algosdk.transaction import (
    ApplicationCreateTxn,
    AssetTransferTxn,
    OnComplete,
    PaymentTxn,
    StateSchema,
    wait_for_confirmation,
)
from algosdk import encoding
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
def localnet_clients(
    algod_client: AlgodClient, kmd_client: KMDClient
) -> au.AlgoSdkClients:
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


@pytest.fixture(scope="session")
def account(localnet_clients: au.AlgoSdkClients) -> SigningAccount:
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


@pytest.fixture(scope="session")
def localnet(
    localnet_clients: au.AlgoSdkClients, account: SigningAccount
) -> au.AlgorandClient:
    client = au.AlgorandClient(localnet_clients)
    client.account.set_signer_from_account(account)
    return client


def load_arc56(name: str) -> au.Arc56Contract:
    """puya-sol writes flat into out/, no per-contract subdirs."""
    arc56_path = OUT_DIR / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


def app_address(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


def fund_account(
    localnet: au.AlgorandClient,
    funder: SigningAccount,
    receiver: SigningAccount | str,
    amount: int,
) -> None:
    algod = localnet.client.algod
    recv_addr = receiver.address if hasattr(receiver, "address") else receiver
    sp = algod.suggested_params()
    txn = PaymentTxn(funder.address, sp, recv_addr, amount)
    signed = txn.sign(funder.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def opt_in_to_asa(
    localnet: au.AlgorandClient,
    holder: SigningAccount,
    asa_id: int,
) -> None:
    algod = localnet.client.algod
    sp = algod.suggested_params()
    txn = AssetTransferTxn(
        sender=holder.address,
        sp=sp,
        receiver=holder.address,
        amt=0,
        index=asa_id,
    )
    signed = txn.sign(holder.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def deploy_aerc20(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    fund_amount: int = 1_000_000,
) -> au.AppClient:
    """Deploy an AERC20-derived contract: create → fund → __postInit.

    The contract creates its underlying ASA inside __postInit, so we need
    enough fees for that inner acfg (and slack for tests that follow up
    with clawback transfers).
    """
    algod = localnet.client.algod
    app_spec = load_arc56(name)

    approval = algod.compile((OUT_DIR / f"{name}.approval.teal").read_text())
    clear = algod.compile((OUT_DIR / f"{name}.clear.teal").read_text())
    approval_program = encoding.base64.b64decode(approval["result"])
    clear_program = encoding.base64.b64decode(clear["result"])

    sp = algod.suggested_params()
    create_txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    signed = create_txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    fund_account(localnet, account, app_address(app_id), fund_amount)

    client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=account.address,
        )
    )

    # __postInit fires the acfg; bump fee so it covers the inner txn.
    client.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[],
            extra_fee=au.AlgoAmount.from_micro_algo(2_000),
        )
    )
    return client
