import hashlib
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.kmd import KMDClient
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
    PaymentTxn,
)
from algosdk.v2client.algod import AlgodClient
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
    client.set_suggested_params_cache_timeout(0)
    client.account.set_signer_from_account(account)
    return client


def load_arc56(name: str, subdir: str | None = None) -> au.Arc56Contract:
    if subdir:
        arc56_path = OUT_DIR / subdir / f"{name}.arc56.json"
    else:
        arc56_path = OUT_DIR / f"{name}Test" / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
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
    subdir: str | None = None,
    extra_pages: int = 0,
    fund_amount: int = 1_000_000,
    constructor_args: list[bytes] | None = None,
) -> au.AppClient:
    app_spec = load_arc56(name, subdir)
    algod = localnet.client.algod
    if subdir:
        base = OUT_DIR / subdir
    else:
        base = OUT_DIR / f"{name}Test"

    approval_path = base / f"{name}.approval.teal"
    clear_path = base / f"{name}.clear.teal"
    approval_result = algod.compile(approval_path.read_text())
    clear_result = algod.compile(clear_path.read_text())
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    max_size = max(len(approval_program), len(clear_program))
    needed_pages = max(0, (max_size - 1) // 2048)
    extra_pages = max(extra_pages, needed_pages)

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=32, num_byte_slices=32),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=constructor_args or [],
        extra_pages=extra_pages,
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)

    app_id = result["application-index"]
    client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=account.address,
        )
    )
    if fund_amount > 0:
        fund_contract(localnet, account, app_id, fund_amount)
    return client


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def addr_to_bytes32(addr: str) -> bytes:
    raw = encoding.decode_address(addr)
    return b"\x00" * (32 - len(raw)) + raw


def app_id_to_bytes32(app_id: int) -> bytes:
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def app_id_to_algod_addr(app_id: int) -> str:
    raw = b"\x00" * 24 + app_id.to_bytes(8, "big")
    return encoding.encode_address(raw)


def int_to_bytes32(val: int) -> bytes:
    return val.to_bytes(32, "big")
