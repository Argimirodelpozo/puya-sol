"""
Uniswap V4 PoolManager — Test Configuration

Helpers for deploying the V4 PoolManager orchestrator and its 53 helper contracts.
The PoolManager is split across 54 AVM programs via the contract splitter.
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
    "PoolManager__Helper1": 2,
    "PoolManager__Helper2": 1,
    "PoolManager__Helper34": 1,
    "PoolManager__Helper49": 1,
    "PoolManager__Helper50": 1,
}

# Minimal always-approve TEAL for budget padding
BUDGET_PAD_TEAL = "#pragma version 10\npushint 1"


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
    orch_app_id: int = 0,
    prev_chunk_app_id: int = 0,
    prev_selector: bytes = b'',
) -> au.AppClient:
    """Deploy a V4 helper contract (or the orchestrator).

    For helper contracts, pass orch_app_id to set the auth state.
    Deploys via bare_create, then calls __init__(o, p, s) to store auth state.
    """
    app_spec = load_arc56(name)
    extra_pages = EXTRA_PAGES.get(name, 0)
    client = deploy_raw(localnet, account, name, app_spec, extra_pages=extra_pages)

    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)

    # Initialize auth state via __init__ ABI method (if this is a helper)
    if orch_app_id > 0:
        init_auth_state(client, orch_app_id, prev_chunk_app_id, prev_selector)

    return client


def init_auth_state(client: au.AppClient, orch_app_id: int,
                    prev_chunk_app_id: int = 0, prev_selector: bytes = b''):
    """Call __init__(uint64,uint64,byte[])void to store auth state on a helper."""
    client.send.call(au.AppClientMethodCallParams(
        method="__init__",
        args=[orch_app_id, prev_chunk_app_id, prev_selector],
    ))


def deploy_budget_pad(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> int:
    """Deploy a minimal always-approve contract for opcode budget pooling.
    Returns the app_id."""
    algod = localnet.client.algod
    approval_result = algod.compile(BUDGET_PAD_TEAL)
    clear_result = algod.compile(BUDGET_PAD_TEAL)
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)

    from algosdk.transaction import wait_for_confirmation
    result = wait_for_confirmation(algod, txid, 4)
    return result["application-index"]


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


# ─── Budget pad fixture ──────────────────────────────────────────────────────

@pytest.fixture(scope="session")
def budget_pad_id(localnet: au.AlgorandClient, account: SigningAccount) -> int:
    """Deploy a minimal always-approve contract for opcode budget pooling."""
    return deploy_budget_pad(localnet, account)


# ─── Helper fixtures ─────────────────────────────────────────────────────────
# Each fixture deploys a specific helper contract at module scope.

@pytest.fixture(scope="module")
def orchestrator(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager")


@pytest.fixture(scope="module")
def helper1(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper1", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper2(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper2", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper3(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper3", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper4(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper4", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper5(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper5", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper6(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper6", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper7(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper7", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper8(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper8", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper9(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper9", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper10(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper10", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper11(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper11", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper12(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper12", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper13(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper13", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper14(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper14", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper15(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper15", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper16(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper16", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper17(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper17", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper18(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper18", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper19(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper19", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper20(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper20", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper21(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper21", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper22(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper22", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper23(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper23", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper24(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper24", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper25(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper25", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper26(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper26", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper27(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper27", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper28(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper28", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper29(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper29", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper30(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper30", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper31(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper31", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper32(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper32", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper33(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper33", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper34(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper34", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper35(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper35", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper36(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper36", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper37(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper37", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper38(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper38", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper39(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper39", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper40(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper40", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper41(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper41", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper42(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper42", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper43(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper43", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper44(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper44", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper45(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper45", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper46(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper46", orch_app_id=orchestrator.app_id)

@pytest.fixture(scope="module")
def helper47(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper47", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper48(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper48", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper49(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper49", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper50(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper50", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper51(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper51", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper52(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper52", orch_app_id=orchestrator.app_id)


@pytest.fixture(scope="module")
def helper53(localnet: au.AlgorandClient, account: SigningAccount, orchestrator: au.AppClient) -> au.AppClient:
    return deploy_helper(localnet, account, "PoolManager__Helper53", orch_app_id=orchestrator.app_id)



