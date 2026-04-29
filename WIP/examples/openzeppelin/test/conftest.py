import hashlib
from pathlib import Path

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema
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
    arc56_path = OUT_DIR / name / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    """Compute the box key for a Solidity mapping.

    Box key = mapping_name_bytes + sha256(concat(keys))
    For nested mappings, keys are concatenated before hashing.
    """
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def fund_account(
    localnet: au.AlgorandClient,
    funder: SigningAccount,
    receiver: SigningAccount | str,
    amount: int = 10_000_000,
) -> None:
    """Fund an account with ALGO from the funder account."""
    from algosdk.transaction import PaymentTxn, wait_for_confirmation
    algod = localnet.client.algod
    recv_addr = receiver.address if hasattr(receiver, 'address') else receiver
    sp = algod.suggested_params()
    txn = PaymentTxn(funder.address, sp, recv_addr, amount)
    signed = txn.sign(funder.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
    """Fund a contract's account with ALGO for box storage MBR."""
    from algosdk.transaction import PaymentTxn, wait_for_confirmation
    algod = localnet.client.algod
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    sp = algod.suggested_params()
    txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def deploy_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    extra_pages: int = 0,
    fund_amount: int = 1_000_000,
    boxes: list[tuple[int, bytes]] | None = None,
    post_init_boxes: list | None = None,
) -> au.AppClient:
    """Deploy a contract via raw create, fund for box MBR, and return client.

    If post_init_boxes is provided, the compiler auto-generated a __postInit
    method (constructor writes to box storage). Deploy sequence:
      1. Create app (no box refs)
      2. Fund app for MBR
      3. Call __postInit() with box references
    """
    app_spec = load_arc56(name)
    client = deploy_contract_raw(
        localnet, account, name, app_spec,
        extra_pages=extra_pages, boxes=boxes,
    )
    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)
    if post_init_boxes is not None:
        # Resolve box references that need app_id
        resolved_boxes = []
        for box in post_init_boxes:
            if callable(box):
                resolved_boxes.append(box(client.app_id))
            else:
                resolved_boxes.append(box)
        client.send.call(
            au.AppClientMethodCallParams(
                method="__postInit",
                args=[],
                box_references=resolved_boxes,
            )
        )
    return client


def deploy_contract_raw(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    app_spec: au.Arc56Contract,
    app_args: list[bytes] | None = None,
    extra_pages: int = 0,
    boxes: list[tuple[int, bytes]] | None = None,
) -> au.AppClient:
    """Deploy a contract with raw ApplicationCreateTxn (avoids duplicate txn issue)."""
    from algosdk.transaction import wait_for_confirmation
    algod = localnet.client.algod

    approval_path = OUT_DIR / name / f"{name}.approval.teal"
    clear_path = OUT_DIR / name / f"{name}.clear.teal"
    approval_result = algod.compile(approval_path.read_text())
    clear_result = algod.compile(clear_path.read_text())
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [],
        extra_pages=extra_pages,
        boxes=boxes or [],
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)

    app_id = result["application-index"]
    return au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=account.address,
        )
    )
