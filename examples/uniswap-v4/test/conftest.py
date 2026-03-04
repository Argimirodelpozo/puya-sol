"""
Uniswap V4 PoolManager — Test Configuration

Helpers for deploying the V4 PoolManager orchestrator and its 45 helper contracts.
The PoolManager is split across 46 AVM programs via the contract splitter.
"""
from pathlib import Path

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema
from algosdk import encoding
from algokit_utils.models.account import SigningAccount
import pytest

OUT_DIR = Path(__file__).parent.parent / "out" / "PoolManager"

# Extra pages required per contract (binary > 2048 bytes)
EXTRA_PAGES = {
    "PoolManager__Helper1": 3,
    "PoolManager__Helper2": 1,
    "PoolManager__Helper3": 1,
}


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
    arc56_path = OUT_DIR / f"{name}.arc56.json"
    return au.Arc56Contract.from_json(arc56_path.read_text())


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
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


def deploy_helper(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    fund_amount: int = 1_000_000,
) -> au.AppClient:
    """Deploy a V4 helper contract (or the orchestrator)."""
    app_spec = load_arc56(name)
    extra_pages = EXTRA_PAGES.get(name, 0)
    client = deploy_raw(localnet, account, name, app_spec, extra_pages=extra_pages)

    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)
    return client


def deploy_raw(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    app_spec: au.Arc56Contract,
    extra_pages: int = 0,
) -> au.AppClient:
    algod = localnet.client.algod
    approval_path = OUT_DIR / f"{name}.approval.teal"
    clear_path = OUT_DIR / f"{name}.clear.teal"
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
        extra_pages=extra_pages,
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)

    from algosdk.transaction import wait_for_confirmation
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


# ─── Helper fixtures ─────────────────────────────────────────────────────────
# Each fixture deploys a specific helper contract at module scope.

@pytest.fixture(scope="module")
def orchestrator(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager")


@pytest.fixture(scope="module")
def helper1(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper1")


@pytest.fixture(scope="module")
def helper2(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper2")


@pytest.fixture(scope="module")
def helper3(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper3")


@pytest.fixture(scope="module")
def helper4(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper4")


@pytest.fixture(scope="module")
def helper5(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper5")


@pytest.fixture(scope="module")
def helper6(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper6")


@pytest.fixture(scope="module")
def helper7(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper7")


@pytest.fixture(scope="module")
def helper8(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper8")


@pytest.fixture(scope="module")
def helper9(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper9")


@pytest.fixture(scope="module")
def helper10(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper10")


@pytest.fixture(scope="module")
def helper11(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper11")


@pytest.fixture(scope="module")
def helper12(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper12")


@pytest.fixture(scope="module")
def helper13(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper13")


@pytest.fixture(scope="module")
def helper14(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper14")


@pytest.fixture(scope="module")
def helper15(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper15")


@pytest.fixture(scope="module")
def helper16(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper16")


@pytest.fixture(scope="module")
def helper17(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper17")


@pytest.fixture(scope="module")
def helper18(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper18")


@pytest.fixture(scope="module")
def helper19(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper19")


@pytest.fixture(scope="module")
def helper20(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper20")


@pytest.fixture(scope="module")
def helper21(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper21")


@pytest.fixture(scope="module")
def helper22(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper22")


@pytest.fixture(scope="module")
def helper23(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper23")


@pytest.fixture(scope="module")
def helper24(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper24")


@pytest.fixture(scope="module")
def helper25(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper25")


@pytest.fixture(scope="module")
def helper26(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper26")


@pytest.fixture(scope="module")
def helper27(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper27")


@pytest.fixture(scope="module")
def helper28(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper28")


@pytest.fixture(scope="module")
def helper29(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper29")


@pytest.fixture(scope="module")
def helper30(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper30")


@pytest.fixture(scope="module")
def helper31(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper31")


@pytest.fixture(scope="module")
def helper32(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper32")


@pytest.fixture(scope="module")
def helper33(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper33")


@pytest.fixture(scope="module")
def helper34(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper34")


@pytest.fixture(scope="module")
def helper35(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper35")


@pytest.fixture(scope="module")
def helper36(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper36")


@pytest.fixture(scope="module")
def helper37(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper37")


@pytest.fixture(scope="module")
def helper38(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper38")


@pytest.fixture(scope="module")
def helper39(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper39")


@pytest.fixture(scope="module")
def helper40(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper40")


@pytest.fixture(scope="module")
def helper41(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper41")


@pytest.fixture(scope="module")
def helper42(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper42")


@pytest.fixture(scope="module")
def helper43(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper43")


@pytest.fixture(scope="module")
def helper44(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper44")


@pytest.fixture(scope="module")
def helper45(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper45")


@pytest.fixture(scope="module")
def helper46(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper46")

@pytest.fixture(scope="module")
def helper47(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper47")

