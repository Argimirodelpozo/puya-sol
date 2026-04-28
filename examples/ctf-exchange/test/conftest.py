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


# Split-CTFExchange fixtures: deploy the CTFExchange__Helper1 helper that
# SimpleSplitter peels off. Used by both test_split_ctfexchange.py (full
# orch+helper integration) and test_calculator_helper.py (helper-only math).
SPLIT_DIR = OUT_DIR / "exchange" / "CTFExchange"
HELPER_DIR = SPLIT_DIR / "CTFExchange__Helper1"


def _compile_teal(algod, teal_text: str) -> bytes:
    return encoding.base64.b64decode(algod.compile(teal_text)["result"])


ORCH_DIR = SPLIT_DIR / "CTFExchange"
TMPL_VAR = "TMPL_CTFExchange__Helper1_APP_ID"


def _create_app(localnet, sender, approval: bytes, clear: bytes, schema,
                app_args=None, fund_amount=5_000_000) -> int:
    algod = localnet.client.algod
    extra_pages = max(0, (max(len(approval), len(clear)) - 1) // 2048)
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=schema.ints, num_byte_slices=schema.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=app_args or [],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]
    if fund_amount > 0:
        sp2 = algod.suggested_params()
        pay = PaymentTxn(sender.address, sp2, algod_addr_for_app(app_id), fund_amount)
        wait_for_confirmation(algod, algod.send_transaction(pay.sign(sender.private_key)), 4)
    return app_id


@pytest.fixture(scope="function")
def split_exchange(localnet, admin):
    """Deploy helper, substitute its app id into the orchestrator's TEAL,
    deploy orchestrator, run __postInit. Returns (helper_client, orch_client).

    The CTFExchange ctor creates 5 boxes + writes to 2 mappings + does an
    inner-call to ERC20.approve — exceeds one txn's resource budget. We
    pre-pad the call with 6 noop app-calls to expand the box-ref slots.
    """
    algod = localnet.client.algod

    helper_spec = au.Arc56Contract.from_json(
        (HELPER_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    helper_approval = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    helper_clear = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.clear.teal").read_text())
    helper_app_id = _create_app(localnet, admin, helper_approval, helper_clear,
                                helper_spec.state.schema.global_state)

    orch_spec = au.Arc56Contract.from_json(
        (ORCH_DIR / "CTFExchange.arc56.json").read_text())
    orch_approval_teal = (ORCH_DIR / "CTFExchange.approval.teal").read_text()
    orch_clear_teal = (ORCH_DIR / "CTFExchange.clear.teal").read_text()
    orch_approval_teal = orch_approval_teal.replace(TMPL_VAR, str(helper_app_id))

    orch_approval = _compile_teal(algod, orch_approval_teal)
    orch_clear = _compile_teal(algod, orch_clear_teal)

    collateral = deploy_app(
        localnet, admin,
        OUT_DIR / "dev" / "mocks" / "ERC20",
        "ERC20",
        post_init_args=["USDC Mock", "USDC"],
    )

    orch_app_id = _create_app(localnet, admin, orch_approval, orch_clear,
                              orch_spec.state.schema.global_state,
                              fund_amount=10_000_000)

    helper_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=helper_spec,
        app_id=helper_app_id, default_sender=admin.address,
    ))
    orch_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec,
        app_id=orch_app_id, default_sender=admin.address,
    ))

    coll_addr = app_id_to_address(collateral.app_id)
    composer = localnet.new_group()
    for i in range(6):
        composer.add_app_call_method_call(orch_client.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin", args=[ZERO_ADDR],
                note=f"pad-{i}".encode(),
            )))
    composer.add_app_call_method_call(orch_client.params.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[coll_addr, coll_addr, ZERO_ADDR, ZERO_ADDR],
            extra_fee=au.AlgoAmount(micro_algo=20_000),
            app_references=[collateral.app_id],
        )))
    composer.send(AUTO_POPULATE)
    return helper_client, orch_client


@pytest.fixture(scope="function")
def helper_only(localnet, admin):
    """Deploy CTFExchange__Helper1 standalone — no TMPL substitution. Use
    this for helper-direct tests (CalculatorHelper math, etc.)."""
    algod = localnet.client.algod
    spec = au.Arc56Contract.from_json(
        (HELPER_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    approval = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    clear = _compile_teal(algod,
        (HELPER_DIR / "CTFExchange__Helper1.clear.teal").read_text())

    extra_pages = max(0, (max(len(approval), len(clear)) - 1) // 2048)
    sch = spec.state.schema.global_state
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=admin.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=sch.ints, num_byte_slices=sch.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
    )
    txid = algod.send_transaction(txn.sign(admin.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]
    sp2 = algod.suggested_params()
    pay = PaymentTxn(admin.address, sp2, algod_addr_for_app(app_id), 5_000_000)
    wait_for_confirmation(algod, algod.send_transaction(pay.sign(admin.private_key)), 4)
    return au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec,
        app_id=app_id, default_sender=admin.address,
    ))


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
