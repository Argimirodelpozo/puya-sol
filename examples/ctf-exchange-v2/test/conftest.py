"""Shared test infrastructure for ctf-exchange v2 translated tests.

The v2 CTFExchange unsplit is ~10.5KB — over AVM's 8192-byte cap. With the
SimpleSplitter's multi-helper mode + --force-delegate matchOrders, the
orchestrator drops to ~5.3KB and deploys cleanly. Two normal helpers
(__Helper1, __Helper2) carry the solady utilities and the in-source pure
mixin helpers; __Helper3 is the delegate sidecar for matchOrders (the
hand-crafted lonely chunk artifact lives in `delegate/CTFExchange__Helper3/`
once the lonely-chunk pass is wired up — until then matchOrders-exercising
tests are xfail).

Fixtures:
  * Smaller contracts (Collateral*, CtfAdapter, NegRiskAdapter): deployed
    standalone with ZERO_ADDR placeholders for cross-contract refs.
  * `split_exchange`: deploys helpers, substitutes their app ids into the
    orchestrator's TEAL, deploys orch, runs __postInit. Returns
    (helper1_client, helper2_client, orch_client).
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
AUTO_POPULATE = au.SendParams(populate_app_call_resources=True)
ZERO_ADDR = b"\x00" * 32


# ── Localnet fixtures ─────────────────────────────────────────────────────

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
    acct = localnet.account.random()
    algod = localnet.client.algod
    sp = algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    txid = algod.send_transaction(pay.sign(admin.private_key))
    wait_for_confirmation(algod, txid, 4)
    return acct


# ── Helpers ───────────────────────────────────────────────────────────────

def addr(account_or_sk) -> bytes:
    return encoding.decode_address(account_or_sk.address)


def algod_addr_for_app(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))


def app_id_to_address(app_id: int) -> bytes:
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def _load_arc56(p: Path) -> au.Arc56Contract:
    return au.Arc56Contract.from_json(p.read_text())


def deploy_app(localnet, sender, base_dir: Path, name: str,
               post_init_args=None, post_init_app_refs=None,
               create_args=None, create_boxes=None, create_apps=None,
               fund_amount=10_000_000, extra_fee=0) -> au.AppClient:
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
        raise RuntimeError(f"{name}: {max_size}B > AVM 8192-byte cap")

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
        boxes=create_boxes or [],
        foreign_apps=create_apps or [],
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
            extra_fee=au.AlgoAmount(micro_algo=20_000),
            app_references=post_init_app_refs or [],
        ), send_params=AUTO_POPULATE)

    return client


# ── Fixtures: each v2 contract that fits, deployed standalone ─────────────
#
# Dependencies between contracts are stubbed with ZERO_ADDR. This means
# wrap/unwrap/match flows won't function (they'd inner-call into nothing),
# but admin/getter tests run against real on-chain state.

# ── Shared mock ERC20 for placeholder token slots ─────────────────────────
# v2's adapters/ramps call `IERC20(token).approve(...)` in their constructors.
# Passing ZERO_ADDR makes those inner calls hit app id 0 (invalid). We reuse
# v1's compiled ERC20 mock as a standalone "shared token" that responds to
# approve / balanceOf / etc.

V1_ERC20_BASE = OUT_DIR.parent.parent / "ctf-exchange" / "out" / "dev" / "mocks" / "ERC20"


@pytest.fixture(scope="session")
def mock_token(localnet, admin):
    return deploy_app(
        localnet, admin, V1_ERC20_BASE, "ERC20",
        post_init_args=["Mock", "MOCK"],
    )


@pytest.fixture(scope="session")
def universal_mock(localnet, admin):
    """Stand-in token contract that no-ops every ERC20/ERC1155 method the
    v2 exchange constructor calls. Used to populate the collateral/ctf
    slots so that `approve` / `setApprovalForAll` inner-calls don't err."""
    base = OUT_DIR / "test" / "dev" / "mocks" / "UniversalMock"
    return deploy_app(localnet, admin, base, "UniversalMock")


@pytest.fixture(scope="function")
def collateral_token(localnet, admin):
    """Deploy CollateralToken. Constructor takes 3 immutable addrs at create
    time (USDC, USDCE, VAULT) via ApplicationArgs and creates the
    `__dyn_storage` box (so we declare it on the create txn). Then
    `initialize(owner)` sets the proxy owner."""
    base = OUT_DIR / "collateral" / "CollateralToken"
    # 8192-byte box needs 8 × 1024B box references for the write budget.
    box_refs = [(0, b"__dyn_storage")] * 8
    client = deploy_app(
        localnet, admin, base, "CollateralToken",
        create_args=[ZERO_ADDR, ZERO_ADDR, ZERO_ADDR],
        create_boxes=box_refs,
    )
    client.send.call(au.AppClientMethodCallParams(
        method="initialize", args=[addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
        box_references=[au.BoxReference(app_id=0, name=b"__dyn_storage")],
    ), send_params=NO_POPULATE)
    return client


@pytest.fixture(scope="function")
def collateral_onramp(localnet, admin, mock_token):
    """CollateralOnramp.__postInit(_collateralToken, _usdc, _usdce). Uses the
    shared mock ERC20 for all three slots so the ctor's `approve` inner-calls
    have a valid receiver."""
    base = OUT_DIR / "collateral" / "CollateralOnramp"
    tok = app_id_to_address(mock_token.app_id)
    return deploy_app(
        localnet, admin, base, "CollateralOnramp",
        post_init_args=[tok, tok, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def collateral_offramp(localnet, admin, mock_token):
    base = OUT_DIR / "collateral" / "CollateralOfframp"
    tok = app_id_to_address(mock_token.app_id)
    return deploy_app(
        localnet, admin, base, "CollateralOfframp",
        post_init_args=[tok, tok, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def permissioned_ramp(localnet, admin, mock_token):
    base = OUT_DIR / "collateral" / "PermissionedRamp"
    tok = app_id_to_address(mock_token.app_id)
    return deploy_app(
        localnet, admin, base, "PermissionedRamp",
        post_init_args=[tok, tok, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def ctf_adapter(localnet, admin, mock_token):
    base = OUT_DIR / "adapters" / "CtfCollateralAdapter"
    tok = app_id_to_address(mock_token.app_id)
    return deploy_app(
        localnet, admin, base, "CtfCollateralAdapter",
        post_init_args=[tok, tok, tok, tok, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def negrisk_adapter(localnet, admin, mock_token):
    base = OUT_DIR / "adapters" / "NegRiskCtfCollateralAdapter"
    tok = app_id_to_address(mock_token.app_id)
    return deploy_app(
        localnet, admin, base, "NegRiskCtfCollateralAdapter",
        post_init_args=[tok, tok, tok, tok, tok, tok],
        post_init_app_refs=[mock_token.app_id],
    )


# ── Split CTFExchange (orchestrator + helpers) ────────────────────────────

SPLIT_DIR = OUT_DIR / "exchange" / "CTFExchange"
H1_DIR = SPLIT_DIR / "CTFExchange__Helper1"
H2_DIR = SPLIT_DIR / "CTFExchange__Helper2"
ORCH_DIR = SPLIT_DIR / "CTFExchange"
TMPL_H1 = "TMPL_CTFExchange__Helper1_APP_ID"
TMPL_H2 = "TMPL_CTFExchange__Helper2_APP_ID"
TMPL_H3 = "TMPL_CTFExchange__Helper3_APP_ID"


def _compile_teal(algod, teal_text: str) -> bytes:
    return encoding.base64.b64decode(algod.compile(teal_text)["result"])


def _inject_memory_init(teal: str) -> str:
    """puya-sol's split helpers don't get the simulated-EVM memory buffer
    initialised in their main routine — only the orchestrator's main gets
    that, because puya only emits the buffer-init for contracts whose
    AWST has a constructor. Methods we extract into the helper still load
    from scratch 0 expecting the buffer, so we prepend the init manually.

    Pattern matches the orchestrator's memory-init prologue verbatim.
    """
    init = (
        "    pushint 4096\n"
        "    bzero\n"
        "    dup\n"
        "    store 5\n"
        "    store 0\n"
        "    load 0\n"
        "    pushbytes 0x0000000000000000000000000000000000000000000000000000000000000080\n"
        "    replace2 64\n"
        "    store 0\n"
    )
    # Insert after the bytecblock and before `txn ApplicationID` (the
    # entry to the router branch). bytecblock can be on a single very
    # long line.
    lines = teal.splitlines()
    out = []
    inserted = False
    for ln in lines:
        out.append(ln)
        if not inserted and ln.lstrip().startswith("bytecblock"):
            out.append(init.rstrip("\n"))
            inserted = True
    return "\n".join(out) + "\n"


def _create_app(localnet, sender, approval: bytes, clear: bytes, schema,
                app_args=None, fund_amount=10_000_000) -> int:
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
def helper1(localnet, admin):
    """Deploy CTFExchange__Helper1 standalone (no orchestrator). Used for
    library-level tests (CalculatorHelper, PolyProxyLib, PolySafeLib,
    CTHelpers) that exercise pure-math helpers without the full exchange.
    """
    algod = localnet.client.algod
    spec = au.Arc56Contract.from_json(
        (H1_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    teal = _inject_memory_init(
        (H1_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    app_id = _create_app(
        localnet, admin,
        _compile_teal(algod, teal),
        _compile_teal(algod, (H1_DIR / "CTFExchange__Helper1.clear.teal").read_text()),
        spec.state.schema.global_state)
    return au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=app_id,
        default_sender=admin.address))


@pytest.fixture(scope="function")
def split_exchange(localnet, admin, universal_mock):
    """Deploy helper1 + helper2 + orchestrator (with helper app ids substituted
    into orch's TEAL). Runs __postInit with `mock_token` filling the slots
    that the constructor calls `approve` / `setApprovalForAll` against
    (collateral, ctf, ctfCollateral, outcomeTokenFactory) — others stay zero.
    Returns (helper1, helper2, orch) clients.

    matchOrders is currently delegated to a hand-crafted lonely-chunk that
    isn't wired up yet — that path xfails until the runtime lands. Auth /
    registration / hashOrder paths work normally on the orchestrator.
    """
    algod = localnet.client.algod

    h1_spec = au.Arc56Contract.from_json(
        (H1_DIR / "CTFExchange__Helper1.arc56.json").read_text())
    h1_approval_teal = _inject_memory_init(
        (H1_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    h1_app_id = _create_app(
        localnet, admin,
        _compile_teal(algod, h1_approval_teal),
        _compile_teal(algod, (H1_DIR / "CTFExchange__Helper1.clear.teal").read_text()),
        h1_spec.state.schema.global_state)

    h2_spec = au.Arc56Contract.from_json(
        (H2_DIR / "CTFExchange__Helper2.arc56.json").read_text())
    h2_approval_teal = _inject_memory_init(
        (H2_DIR / "CTFExchange__Helper2.approval.teal").read_text())
    h2_approval_teal = h2_approval_teal.replace(TMPL_H1, str(h1_app_id))
    h2_app_id = _create_app(
        localnet, admin,
        _compile_teal(algod, h2_approval_teal),
        _compile_teal(algod, (H2_DIR / "CTFExchange__Helper2.clear.teal").read_text()),
        h2_spec.state.schema.global_state)

    orch_spec = au.Arc56Contract.from_json(
        (ORCH_DIR / "CTFExchange.arc56.json").read_text())
    orch_approval_teal = (ORCH_DIR / "CTFExchange.approval.teal").read_text()
    orch_approval_teal = orch_approval_teal.replace(TMPL_H1, str(h1_app_id))
    orch_approval_teal = orch_approval_teal.replace(TMPL_H2, str(h2_app_id))
    # H3 (matchOrders delegate sidecar) isn't deployed yet — substitute 0
    # so the orch's TEAL compiles. matchOrders calls fail at runtime.
    orch_approval_teal = orch_approval_teal.replace(TMPL_H3, "0")

    orch_approval = _compile_teal(algod, orch_approval_teal)
    orch_clear = _compile_teal(algod, (ORCH_DIR / "CTFExchange.clear.teal").read_text())

    orch_app_id = _create_app(localnet, admin, orch_approval, orch_clear,
                              orch_spec.state.schema.global_state)

    h1_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h1_spec, app_id=h1_app_id,
        default_sender=admin.address))
    h2_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h2_spec, app_id=h2_app_id,
        default_sender=admin.address))
    orch_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec, app_id=orch_app_id,
        default_sender=admin.address))

    # ExchangeInitParams: 8 addresses packed as (admin, collateral, ctf,
    # ctfCollateral, outcomeTokenFactory, proxyFactory, safeFactory,
    # feeReceiver). The arc56 type is uint8[32] per field; algosdk wants
    # list[int] for that, not bytes. The constructor fires inner-app calls
    # against `collateral` (ERC20.approve), `ctf` (ERC1155.setApprovalForAll
    # — which our compiled ERC20 mock simply ignores), so those four slots
    # have to be a deployed app. proxy/safe/fee receiver stay zero — the
    # paths that read them fail at runtime, marked xfail per-test.
    tok_addr = list(app_id_to_address(universal_mock.app_id))
    init_params = [
        list(addr(admin)),  # admin
        tok_addr,           # collateral (ERC20.approve)
        tok_addr,           # ctf (ERC1155.setApprovalForAll)
        tok_addr,           # ctfCollateral
        tok_addr,           # outcomeTokenFactory
        tok_addr,           # proxyFactory (.getImplementation)
        tok_addr,           # safeFactory (.proxyCreationCode)
        list(addr(admin)),  # feeReceiver — any address; admin works
    ]

    composer = localnet.new_group()
    for i in range(6):
        composer.add_app_call_method_call(orch_client.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin", args=[ZERO_ADDR],
                note=f"pad-{i}".encode())))
    composer.add_app_call_method_call(orch_client.params.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[init_params],
            extra_fee=au.AlgoAmount(micro_algo=50_000),
            app_references=[h1_app_id, h2_app_id, universal_mock.app_id])))
    composer.send(AUTO_POPULATE)
    return h1_client, h2_client, orch_client
