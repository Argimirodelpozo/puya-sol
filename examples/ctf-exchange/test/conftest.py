"""Shared test infrastructure for ctf-exchange v1 translated tests.

Note: v1 CTFExchange.sol compiles but the resulting TEAL (~9.1KB) exceeds
Algorand's 8192-byte cap (1 base + 3 extra pages). Until puya-sol's contract
splitter is wired up, only the smaller helpers (ERC20, ERC1271Mock) are
deployable on AVM. Those are tested here.
"""
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
import pytest

OUT_DIR = Path(__file__).parent.parent / "out"
NO_POPULATE = au.SendParams(populate_app_call_resources=False)
# Auto-populates Boxes/Apps/Accounts/Assets references on dispatch — needed
# whenever a method touches mapping storage (puya-sol stores all mappings as
# boxes). Adds a small per-call setup cost; fine for tests.
AUTO_POPULATE = au.SendParams(populate_app_call_resources=True)
ZERO_ADDR = b"\x00" * 32


@pytest.fixture(scope="session")
def algod_client() -> AlgodClient:
    return au.ClientManager.get_algod_client(au.ClientManager.get_default_localnet_config("algod"))


@pytest.fixture(scope="session")
def kmd_client() -> KMDClient:
    return au.ClientManager.get_kmd_client(au.ClientManager.get_default_localnet_config("kmd"))


@pytest.fixture(scope="session")
def localnet_clients(algod_client, kmd_client):
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


@pytest.fixture(scope="session")
def admin(localnet_clients):
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


@pytest.fixture(scope="session")
def localnet(localnet_clients, admin):
    client = au.AlgorandClient(localnet_clients)
    client.set_suggested_params_cache_timeout(0)
    client.account.set_signer_from_account(admin)
    return client


@pytest.fixture(scope="function")
def funded_account(localnet, admin):
    """Per-test fresh account, funded from admin."""
    acct = localnet.account.random()
    algod = localnet.client.algod
    sp = algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    txid = algod.send_transaction(pay.sign(admin.private_key))
    wait_for_confirmation(algod, txid, 4)
    return acct


def addr(account_or_sk) -> bytes:
    """Return the 32-byte raw address for an algokit/algosdk account."""
    return encoding.decode_address(account_or_sk.address)


def algod_addr_for_app(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))


def app_id_to_address(app_id: int) -> bytes:
    """puya-sol address convention for an Algorand app id (24 zero bytes ++ itob)."""
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def _load_arc56(p: Path) -> au.Arc56Contract:
    return au.Arc56Contract.from_json(p.read_text())


def deploy_app(localnet, sender, base_dir: Path, name: str,
               post_init_args=None, post_init_app_refs=None,
               create_args=None,
               fund_amount=1_000_000, extra_fee=0) -> au.AppClient:
    """Deploy a puya-sol-emitted app and run its __postInit if args supplied.

    base_dir is the directory containing <name>.approval.teal / <name>.clear.teal /
    <name>.arc56.json.
    """
    spec = _load_arc56(base_dir / f"{name}.arc56.json")
    algod = localnet.client.algod
    sch = spec.state.schema.global_state

    approval_bin = encoding.base64.b64decode(
        algod.compile((base_dir / f"{name}.approval.teal").read_text())["result"])
    clear_bin = encoding.base64.b64decode(
        algod.compile((base_dir / f"{name}.clear.teal").read_text())["result"])

    max_size = max(len(approval_bin), len(clear_bin))
    extra_pages = max(0, (max_size - 1) // 2048)
    if extra_pages > 3:
        raise RuntimeError(
            f"{name}: compiled size {max_size} exceeds AVM 8192-byte cap "
            f"(needs {extra_pages} extra pages, max 3)")

    sp = algod.suggested_params()
    sp.fee = max(sp.min_fee, sp.fee) + extra_fee
    sp.flat_fee = True
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin, clear_program=clear_bin,
        global_schema=StateSchema(num_uints=sch.ints, num_byte_slices=sch.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=create_args or [],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    if fund_amount > 0:
        sp2 = algod.suggested_params()
        pay = PaymentTxn(sender.address, sp2, algod_addr_for_app(app_id), fund_amount)
        wait_for_confirmation(algod, algod.send_transaction(pay.sign(sender.private_key)), 4)

    client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec,
        app_id=app_id, default_sender=sender.address,
    ))

    if post_init_args is not None:
        client.send.call(au.AppClientMethodCallParams(
            method="__postInit",
            args=post_init_args,
            extra_fee=au.AlgoAmount(micro_algo=10_000),
            app_references=post_init_app_refs or [],
        ), send_params=AUTO_POPULATE)

    return client


# ── Per-test contracts (small enough to fit) ──────────────────────────────

@pytest.fixture(scope="function")
def erc20(localnet, admin):
    return deploy_app(
        localnet, admin,
        OUT_DIR / "dev" / "mocks" / "ERC20",
        "ERC20",
        post_init_args=["USDC Mock", "USDC"],
    )


@pytest.fixture(scope="function")
def erc1271_mock(localnet, admin):
    """ERC1271Mock's single-statement constructor (`signer = _signer`) is
    inlined by puya-sol into the approval program's create-time branch; it
    reads ApplicationArgs[0] as the 32-byte signer address.
    """
    return deploy_app(
        localnet, admin,
        OUT_DIR / "dev" / "mocks" / "ERC1271Mock",
        "ERC1271Mock",
        create_args=[addr(admin)],
    )
