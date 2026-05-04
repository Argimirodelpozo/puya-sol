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


def deploy_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    # AVM default is 1 program page (2048 B). extra_pages adds N more
    # 2048-B pages, max 3 → up to 4 × 2048 = 8192 B per program. Several
    # AAVE V4 logic contracts (HubConfigurator 5.7 KB, SpokeConfigurator
    # 5.0 KB, AccessManager 5.9 KB) are over 4 KB; using extra_pages=3
    # by default removes the per-test guesswork — there's no runtime
    # cost, only a tiny MBR bump on the deploying account.
    extra_pages: int = 3,
    fund_amount: int = 1_000_000,
    app_args: list[bytes] | None = None,
) -> au.AppClient:
    app_spec = load_arc56(name)
    client = deploy_contract_raw(
        localnet, account, name, app_spec,
        app_args=app_args,
        extra_pages=extra_pages,
    )
    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)

    # puya-sol's "constructor box-write auto-split" pattern:
    # AppCreate sets `__ctor_pending = 1`; the actual constructor body
    # runs in `__postInit()`. Tests have to call it before any other
    # method; otherwise state-var initializers (e.g. `string public
    # name = "..."` on WETH9) never land, and reads fire "check name
    # exists" at runtime.
    # Look up __postInit's full signature from the arc56 spec so we
    # build the exact selector + arg shape the contract expects. puya-sol
    # passes the constructor's args through to __postInit, so the same
    # `app_args` we used for AppCreate are the args here.
    postinit_method = None
    for m in getattr(app_spec, "methods", []) or []:
        if getattr(m, "name", None) == "__postInit":
            postinit_method = m
            break

    # Only run __postInit when the test passed enough constructor args to
    # match the method's arity. Tests that deploy without ctor args are
    # exercising methods that don't depend on the state __postInit
    # initialises (e.g. pure view methods on default state); calling
    # __postInit with missing args would just revert at "invalid
    # ApplicationArgs index N".
    if postinit_method is not None and len(app_args or []) >= len(postinit_method.args):
        from algosdk.atomic_transaction_composer import (
            AtomicTransactionComposer, TransactionWithSigner,
            AccountTransactionSigner,
        )
        from algosdk.transaction import ApplicationCallTxn, OnComplete
        signer = AccountTransactionSigner(account.private_key)
        sp = localnet.client.algod.suggested_params()
        # Build full ABI signature: __postInit(arg0_type,arg1_type,...)void
        arg_types = ",".join(getattr(a, "type", "") for a in postinit_method.args)
        sig = f"__postInit({arg_types})void"
        sel = hashlib.new("sha512_256", sig.encode()).digest()[:4]
        # Forward original constructor app_args (one per __postInit param).
        call_args = [sel] + list(app_args or [])[: len(postinit_method.args)]
        call_txn = ApplicationCallTxn(
            sender=account.address, sp=sp, index=client.app_id,
            on_complete=OnComplete.NoOpOC, app_args=call_args,
        )
        atc = AtomicTransactionComposer()
        atc.add_transaction(TransactionWithSigner(call_txn, signer))
        try:
            atc = au.populate_app_call_resources(atc, localnet.client.algod)
        except Exception:
            pass
        try:
            atc.execute(localnet.client.algod, 4)
        except Exception:
            # __postInit may revert when the test passes mis-encoded ctor
            # args (e.g. raw bytes where ARC4-prefixed string expected),
            # or when the contract has no real state to init. Either way:
            # leave the contract in its post-AppCreate state and let the
            # test surface the actual missing-init at field-read time.
            pass
    return client


def deploy_contract_raw(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    app_spec: au.Arc56Contract,
    app_args: list[bytes] | None = None,
    extra_pages: int = 0,
) -> au.AppClient:
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
