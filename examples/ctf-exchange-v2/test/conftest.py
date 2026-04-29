"""Top-level pytest fixtures for ctf-exchange v2.

Reusable utility code lives in `test/dev/` (see `test/dev/__init__.py`).
This file holds only fixtures + cross-category fixture composition.

Sub-package layout (in progress):
  test/conftest.py         — this file (localnet, admin, base helpers)
  test/dev/                — utility modules (no fixtures)
  test/collateral/         — CollateralToken, On/Offramp, PermissionedRamp tests
  test/adapters/           — Ctf + NegRisk adapter tests
  test/library/            — pure-math + ECDSA probe tests
  test/exchange/           — orch-dependent tests (currently mostly errors/xfails)
  test/smoke/              — deployment smoke tests

Cross-cutting fixtures (`mock_token`, `universal_mock`, `split_exchange`)
live here for now and migrate down to per-category conftests as the tree
fills in.
"""
from pathlib import Path
import subprocess

import algokit_utils as au
import pytest
from algosdk.transaction import (
    ApplicationCreateTxn,
    OnComplete,
    PaymentTxn,
    StateSchema,
    wait_for_confirmation,
)
from algosdk.kmd import KMDClient
from algosdk.v2client.algod import AlgodClient

from dev.addrs import addr, algod_addr_for_app, app_id_to_address
from dev.arc56 import compile_teal, inject_memory_init, load_arc56
from dev.deploy import AUTO_POPULATE, NO_POPULATE, create_app, deploy_app
from dev.localnet import (
    fund_random_account,
    make_admin_account,
    make_algod_client,
    make_kmd_client,
    make_localnet,
    make_localnet_clients,
)


# Re-exported so tests written before the dev/ refactor still work.
__all__ = [
    "addr",
    "algod_addr_for_app",
    "app_id_to_address",
    "AUTO_POPULATE",
    "NO_POPULATE",
    "ZERO_ADDR",
    "OUT_DIR",
    "deploy_app",
]


OUT_DIR = Path(__file__).parent.parent / "out"
ZERO_ADDR = b"\x00" * 32


# ── Localnet ──────────────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def algod_client() -> AlgodClient:
    return make_algod_client()


@pytest.fixture(scope="session")
def kmd_client() -> KMDClient:
    return make_kmd_client()


@pytest.fixture(scope="session")
def localnet_clients(algod_client, kmd_client):
    return make_localnet_clients(algod_client, kmd_client)


@pytest.fixture(scope="session")
def admin(localnet_clients):
    return make_admin_account(localnet_clients)


@pytest.fixture(scope="session")
def localnet(localnet_clients, admin):
    return make_localnet(localnet_clients, admin)


@pytest.fixture(scope="function")
def funded_account(localnet, admin):
    return fund_random_account(localnet, admin)


# ── Shared mocks ──────────────────────────────────────────────────────────
#
# v2's adapters/ramps call IERC20(token).approve(...) in their constructors.
# We reuse v1's compiled ERC20 mock as a standalone token, and v2's
# UniversalMock for slots that need both ERC20 and ERC1155 surface.

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


# ── Stateful USDC + CTF mocks for matchOrders settlement ────────────────
# These live under delegate/out/ (puyapy-built, not puya-sol). Real
# balances/allowances/approvals/partition tracking — wired into the
# split exchange in place of `universal_mock` for collateral + ctf slots.

_DELEGATE_OUT = Path(__file__).parent.parent / "delegate" / "out"


def _deploy_pyapp(localnet, admin, name: str, *, init_args=None) -> au.AppClient:
    """Deploy a puyapy-emitted app from delegate/out/."""
    spec = au.Arc56Contract.from_json((_DELEGATE_OUT / f"{name}.arc56.json").read_text())
    approval = (_DELEGATE_OUT / f"{name}.approval.bin").read_bytes()
    clear = (_DELEGATE_OUT / f"{name}.clear.bin").read_bytes()

    sch = spec.state.schema.global_state
    extra_pages = max(0, (max(len(approval), len(clear)) - 1) // 2048)
    sp = localnet.client.algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=admin.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=sch.ints, num_byte_slices=sch.bytes),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=init_args or [bytes.fromhex("83f14748")],  # method "init()void" selector
    )
    txid = localnet.client.algod.send_transaction(txn.sign(admin.private_key))
    res = wait_for_confirmation(localnet.client.algod, txid, 4)
    app_id = res["application-index"]

    pay = PaymentTxn(
        admin.address,
        localnet.client.algod.suggested_params(),
        algod_addr_for_app(app_id),
        50_000_000,  # MBR for boxes (balances grow with deals)
    )
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)

    return au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=app_id,
        default_sender=admin.address))


@pytest.fixture(scope="function")
def usdc_stateful(localnet, admin):
    """Stateful USDC mock (delegate/usdc_mock.py): real balances +
    allowances, exposes mint/transfer/transferFrom/balanceOf/allowance/approve."""
    return _deploy_pyapp(localnet, admin, "USDCMock")


@pytest.fixture(scope="function")
def ctf_stateful(localnet, admin):
    """Stateful CTF mock (delegate/ctf_mock.py): real ERC1155 balances +
    approvals + (yes_id, no_id) partitions. Exposes mint/balanceOf/
    setApprovalForAll/safeTransferFrom/splitPosition/mergePositions/
    prepare_condition."""
    return _deploy_pyapp(localnet, admin, "CTFMock")


# ── Standalone-contract fixtures (collateral system) ─────────────────────


@pytest.fixture(scope="function")
def erc1271_mock_factory(localnet, admin):
    """Factory: `erc1271_mock_factory(signer_eth_addr_20)` deploys an
    ERC1271Mock and returns the AppClient.

    The mock holds `signer` (32-byte address slot) and exposes
    `isValidSignature(hash, sig)` which uses the AVM-PORT-ADAPTATION
    r/s/v + ecrecover precompile path. Pass the inner signer's 20-byte
    eth address; we left-pad to 32 bytes for the create-arg.
    """
    base = OUT_DIR / "test" / "dev" / "mocks" / "ERC1271Mock"

    def _deploy(signer_eth20: bytes) -> au.AppClient:
        signer32 = b"\x00" * 12 + signer_eth20 if len(signer_eth20) == 20 else signer_eth20
        return deploy_app(localnet, admin, base, "ERC1271Mock", create_args=[signer32])

    return _deploy


@pytest.fixture(scope="function")
def toggleable_1271_factory(localnet, admin):
    """Like `erc1271_mock_factory` but for ToggleableERC1271Mock — has
    `disable()` to turn off signature validation mid-test."""
    base = OUT_DIR / "test" / "dev" / "mocks" / "ToggleableERC1271Mock"

    def _deploy(signer_eth20: bytes) -> au.AppClient:
        signer32 = b"\x00" * 12 + signer_eth20 if len(signer_eth20) == 20 else signer_eth20
        return deploy_app(localnet, admin, base, "ToggleableERC1271Mock", create_args=[signer32])

    return _deploy


@pytest.fixture(scope="function")
def usdc(localnet, admin):
    """Deploy USDC mock (solady ERC20, 6 decimals, mint/burn)."""
    base = OUT_DIR / "test" / "dev" / "mocks"
    return deploy_app(localnet, admin, base, "USDC", create_args=[])


@pytest.fixture(scope="function")
def usdce(localnet, admin):
    """Deploy USDCe mock (solady ERC20, 6 decimals, mint/burn)."""
    base = OUT_DIR / "test" / "dev" / "mocks"
    return deploy_app(localnet, admin, base, "USDCe", create_args=[])


@pytest.fixture(scope="function")
def vault(localnet, admin):
    """A funded account used as the vault address that holds wrapped collateral."""
    return fund_random_account(localnet, admin)


@pytest.fixture(scope="function")
def collateral_token(localnet, admin):
    """Deploy CollateralToken (UUPS proxy).

    With puya-sol's `initializer`-modifier-hoisting (option (a) from this
    session), `__postInit` now takes 4 args — the constructor's USDC/USDCE/
    VAULT plus `_owner` from `initialize(_owner)`. One call to __postInit
    replaces the previous "initialize then __postInit" sequence and avoids
    the PUYA #1 bug.

    Sequence:
      1. Deploy SafeTransferLib helper (CallContextChecker__Helper1).
      2. Substitute helper app id into orch TEAL, create orch.
      3. `create_app` funds the orch (covers __dyn_storage 8KB MBR).
      4. Call `__postInit(USDC, USDCE, VAULT, owner)` — sets immutables,
         runs the original-ctor body (incl. `_disableInitializers`), then
         the original `initialize(owner)` body wrapped in its `initializer`
         modifier — all in one frame so ABI args flow through correctly.
    """
    base = OUT_DIR / "collateral" / "CollateralToken"
    helper_dir = base / "CallContextChecker__Helper1"
    orch_dir = base / "CollateralToken"
    algod = localnet.client.algod

    h_spec = load_arc56(helper_dir / "CallContextChecker__Helper1.arc56.json")
    h_teal = inject_memory_init(
        (helper_dir / "CallContextChecker__Helper1.approval.teal").read_text())
    h_app_id = create_app(
        localnet, admin,
        compile_teal(algod, h_teal),
        compile_teal(
            algod, (helper_dir / "CallContextChecker__Helper1.clear.teal").read_text()),
        h_spec.state.schema.global_state,
    )

    orch_spec = load_arc56(orch_dir / "CollateralToken.arc56.json")
    orch_teal = (orch_dir / "CollateralToken.approval.teal").read_text().replace(
        "TMPL_CallContextChecker__Helper1_APP_ID", str(h_app_id))
    orch_clear = (orch_dir / "CollateralToken.clear.teal").read_text().replace(
        "TMPL_CallContextChecker__Helper1_APP_ID", str(h_app_id))
    orch_approval_bin = compile_teal(algod, orch_teal)
    orch_clear_bin = compile_teal(algod, orch_clear)

    sch = orch_spec.state.schema.global_state
    extra_pages = max(
        0, (max(len(orch_approval_bin), len(orch_clear_bin)) - 1) // 2048)
    app_id = create_app(
        localnet, admin, orch_approval_bin, orch_clear_bin,
        sch, extra_pages=extra_pages,
        app_args=[ZERO_ADDR, ZERO_ADDR, ZERO_ADDR],
    )

    client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec, app_id=app_id,
        default_sender=admin.address,
    ))

    client.send.call(au.AppClientMethodCallParams(
        method="__postInit",
        args=[ZERO_ADDR, ZERO_ADDR, ZERO_ADDR, addr(admin)],
        extra_fee=au.AlgoAmount(micro_algo=20_000),
        box_references=[au.BoxReference(app_id=0, name=b"__dyn_storage")],
        app_references=[h_app_id],
    ), send_params=AUTO_POPULATE)

    return client


@pytest.fixture(scope="function")
def collateral_onramp(localnet, admin, mock_token):
    base = OUT_DIR / "collateral" / "CollateralOnramp"
    from algosdk import encoding
    tok = encoding.encode_address(app_id_to_address(mock_token.app_id))
    return deploy_app(
        localnet, admin, base, "CollateralOnramp",
        post_init_args=[admin.address, admin.address, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def collateral_offramp(localnet, admin, mock_token):
    base = OUT_DIR / "collateral" / "CollateralOfframp"
    from algosdk import encoding
    tok = encoding.encode_address(app_id_to_address(mock_token.app_id))
    return deploy_app(
        localnet, admin, base, "CollateralOfframp",
        post_init_args=[admin.address, admin.address, tok],
        post_init_app_refs=[mock_token.app_id],
    )


@pytest.fixture(scope="function")
def permissioned_ramp(localnet, admin, mock_token):
    base = OUT_DIR / "collateral" / "PermissionedRamp"
    from algosdk import encoding
    tok = encoding.encode_address(app_id_to_address(mock_token.app_id))
    return deploy_app(
        localnet, admin, base, "PermissionedRamp",
        post_init_args=[admin.address, admin.address, tok],
        post_init_app_refs=[mock_token.app_id],
    )


# ── Standalone-contract fixtures (adapters) ──────────────────────────────


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

DELEGATE_DIR = Path(__file__).parent.parent / "delegate"
LONELY_CHUNK_OUT = DELEGATE_DIR / "out"


def _build_lonely_chunk():
    """Compile delegate/lonely_chunk.py via puyapy. Cached."""
    src = DELEGATE_DIR / "lonely_chunk.py"
    out_bin = LONELY_CHUNK_OUT / "LonelyChunk.approval.bin"
    if out_bin.exists() and out_bin.stat().st_mtime > src.stat().st_mtime:
        return
    PUYA_VENV = Path(__file__).parent.parent.parent.parent / "puya" / ".venv" / "bin" / "puyapy"
    subprocess.run(
        [str(PUYA_VENV), str(src), "--output-bytecode",
         "--target-avm-version", "12",
         "--out-dir", str(LONELY_CHUNK_OUT)],
        check=True, capture_output=True,
    )


@pytest.fixture(scope="session")
def lonely_chunk_artifacts():
    _build_lonely_chunk()
    return {
        "approval_bin": (LONELY_CHUNK_OUT / "LonelyChunk.approval.bin").read_bytes(),
        "clear_bin": (LONELY_CHUNK_OUT / "LonelyChunk.clear.bin").read_bytes(),
        "spec": au.Arc56Contract.from_json(
            (LONELY_CHUNK_OUT / "LonelyChunk.arc56.json").read_text()),
    }


@pytest.fixture(scope="function")
def helper1(localnet, admin):
    """Deploy CTFExchange__Helper1 standalone (no orchestrator). Used for
    library-level tests that exercise pure helpers without the full exchange."""
    algod = localnet.client.algod
    spec = load_arc56(H1_DIR / "CTFExchange__Helper1.arc56.json")
    teal = inject_memory_init(
        (H1_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    app_id = create_app(
        localnet, admin,
        compile_teal(algod, teal),
        compile_teal(algod, (H1_DIR / "CTFExchange__Helper1.clear.teal").read_text()),
        spec.state.schema.global_state)
    return au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=app_id,
        default_sender=admin.address))


@pytest.fixture(scope="function")
def split_exchange(localnet, admin, universal_mock):
    """Deploy helper1 + helper2 + orchestrator. Returns (h1, h2, orch).

    matchOrders is delegated to a hand-crafted lonely chunk that isn't
    fully wired yet — that path xfails per-test until the runtime lands.
    Auth / registration / hashOrder paths work normally.
    """
    algod = localnet.client.algod

    h1_spec = load_arc56(H1_DIR / "CTFExchange__Helper1.arc56.json")
    h1_teal = inject_memory_init(
        (H1_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    h1_app_id = create_app(
        localnet, admin,
        compile_teal(algod, h1_teal),
        compile_teal(algod, (H1_DIR / "CTFExchange__Helper1.clear.teal").read_text()),
        h1_spec.state.schema.global_state)

    h2_spec = load_arc56(H2_DIR / "CTFExchange__Helper2.arc56.json")
    h2_teal = inject_memory_init(
        (H2_DIR / "CTFExchange__Helper2.approval.teal").read_text())
    h2_teal = h2_teal.replace(TMPL_H1, str(h1_app_id))
    h2_app_id = create_app(
        localnet, admin,
        compile_teal(algod, h2_teal),
        compile_teal(algod, (H2_DIR / "CTFExchange__Helper2.clear.teal").read_text()),
        h2_spec.state.schema.global_state)

    orch_spec = load_arc56(ORCH_DIR / "CTFExchange.arc56.json")
    orch_teal = (ORCH_DIR / "CTFExchange.approval.teal").read_text()
    orch_teal = orch_teal.replace(TMPL_H1, str(h1_app_id))
    orch_teal = orch_teal.replace(TMPL_H2, str(h2_app_id))
    orch_teal = orch_teal.replace(TMPL_H3, "0")

    orch_approval = compile_teal(algod, orch_teal)
    orch_clear = compile_teal(
        algod, (ORCH_DIR / "CTFExchange.clear.teal").read_text())

    # extra_pages=3 so the lonely chunk can install up to 8KB onto orch
    # via UpdateApplication later.
    orch_app_id = create_app(localnet, admin, orch_approval, orch_clear,
                             orch_spec.state.schema.global_state,
                             extra_pages=3)

    h1_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h1_spec, app_id=h1_app_id,
        default_sender=admin.address))
    h2_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h2_spec, app_id=h2_app_id,
        default_sender=admin.address))
    orch_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec, app_id=orch_app_id,
        default_sender=admin.address))

    tok_addr = list(app_id_to_address(universal_mock.app_id))
    init_params = [
        list(addr(admin)),
        tok_addr,           # collateral
        tok_addr,           # ctf
        tok_addr,           # ctfCollateral
        tok_addr,           # outcomeTokenFactory
        tok_addr,           # proxyFactory
        tok_addr,           # safeFactory
        list(addr(admin)),  # feeReceiver
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


H3_DIR = SPLIT_DIR / "CTFExchange__Helper3"


def _build_split_exchange(localnet, admin, *, collateral_app_id, ctf_app_id,
                          ctf_collateral_app_id, factory_app_id,
                          outcome_factory_app_id=None):
    """Internal helper. Deploys h1+h2+orch, runs __postInit with the given
    apps for each slot. `factory_app_id` is used for proxyFactory /
    safeFactory (which need getImplementation()/masterCopy() during init)
    AND, by default, for outcomeTokenFactory. For settlement tests that
    drive MINT/MERGE through `_mint`/`_merge` (which inner-call
    `outcomeTokenFactory.splitPosition` / `.mergePositions`), pass the
    real CTF mock app id as `outcome_factory_app_id` so that path can
    actually find the method. The init-time call chain doesn't touch
    outcomeTokenFactory — `Assets`'s ctor only does
    `collateral.approve(otf, max)` and `ctf.setApprovalForAll(otf, true)`,
    both addressed at collateral/ctf, not at otf — so any valid app id
    works for the outcome-factory slot.
    Returns (h1, h2, orch)."""
    algod = localnet.client.algod

    h1_spec = load_arc56(H1_DIR / "CTFExchange__Helper1.arc56.json")
    h1_teal = inject_memory_init(
        (H1_DIR / "CTFExchange__Helper1.approval.teal").read_text())
    h1_app_id = create_app(
        localnet, admin,
        compile_teal(algod, h1_teal),
        compile_teal(algod, (H1_DIR / "CTFExchange__Helper1.clear.teal").read_text()),
        h1_spec.state.schema.global_state)

    h2_spec = load_arc56(H2_DIR / "CTFExchange__Helper2.arc56.json")
    h2_teal = inject_memory_init(
        (H2_DIR / "CTFExchange__Helper2.approval.teal").read_text())
    h2_teal = h2_teal.replace(TMPL_H1, str(h1_app_id))
    h2_app_id = create_app(
        localnet, admin,
        compile_teal(algod, h2_teal),
        compile_teal(algod, (H2_DIR / "CTFExchange__Helper2.clear.teal").read_text()),
        h2_spec.state.schema.global_state)

    orch_spec = load_arc56(ORCH_DIR / "CTFExchange.arc56.json")
    orch_teal = (ORCH_DIR / "CTFExchange.approval.teal").read_text()
    orch_teal = orch_teal.replace(TMPL_H1, str(h1_app_id))
    orch_teal = orch_teal.replace(TMPL_H2, str(h2_app_id))
    orch_teal = orch_teal.replace(TMPL_H3, "0")

    orch_approval = compile_teal(algod, orch_teal)
    orch_clear = compile_teal(
        algod, (ORCH_DIR / "CTFExchange.clear.teal").read_text())
    orch_app_id = create_app(localnet, admin, orch_approval, orch_clear,
                             orch_spec.state.schema.global_state,
                             extra_pages=3)

    h1_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h1_spec, app_id=h1_app_id,
        default_sender=admin.address))
    h2_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=h2_spec, app_id=h2_app_id,
        default_sender=admin.address))
    orch_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=orch_spec, app_id=orch_app_id,
        default_sender=admin.address))

    coll_addr = list(app_id_to_address(collateral_app_id))
    ctf_addr = list(app_id_to_address(ctf_app_id))
    ctf_coll_addr = list(app_id_to_address(ctf_collateral_app_id))
    factory_addr = list(app_id_to_address(factory_app_id))
    otf_app_id = outcome_factory_app_id if outcome_factory_app_id else factory_app_id
    otf_addr = list(app_id_to_address(otf_app_id))
    init_params = [
        list(addr(admin)),
        coll_addr,           # collateral (USDC)
        ctf_addr,            # ctf (ConditionalTokens / ERC1155)
        ctf_coll_addr,       # ctfCollateral
        otf_addr,            # outcomeTokenFactory (CTF for settlement, mock otherwise)
        factory_addr,        # proxyFactory (needs getImplementation())
        factory_addr,        # safeFactory (needs masterCopy())
        list(addr(admin)),   # feeReceiver
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
            app_references=[h1_app_id, h2_app_id, collateral_app_id,
                            ctf_app_id, ctf_collateral_app_id, factory_app_id,
                            otf_app_id])))
    # AVM-PORT-ADAPTATION: helper1 holds the extracted TransferHelper —
    # its CTF inner-calls reach the CTF receiver with `msg.sender ==
    # helper1`, not `address(this)` (the orch). CTFMock's
    # `safeTransferFrom` early-outs when `msg.sender == from`; for
    # orch→user transfers (MINT/MERGE distribute) `from == orch != helper1`,
    # so the approval check fires and asserts "not approved operator".
    # Explicitly grant helper1 setApprovalForAll on the CTF, post-init.
    #
    # Use helper1's REAL Algorand address (sha512_256("appID" || app_id))
    # rather than puya-sol's storage-slot convention (24 zeros + itob(app_id))
    # — when helper1 later calls CTFMock as an inner-tx, `op.Txn.sender`
    # at the receiver is the real algorand address, so the approval box
    # has to be keyed against that.
    from dev.addrs import algod_addr_bytes_for_app
    composer.add_app_call_method_call(orch_client.params.call(
        au.AppClientMethodCallParams(
            method="_avmPortGrantCtfOperator",
            args=[algod_addr_bytes_for_app(h1_app_id)],
            extra_fee=au.AlgoAmount(micro_algo=10_000),
            app_references=[ctf_app_id, h1_app_id])))
    composer.send(AUTO_POPULATE)
    return h1_client, h2_client, orch_client


@pytest.fixture(scope="function")
def split_exchange_settled(localnet, admin, universal_mock, usdc_stateful, ctf_stateful):
    """Same shape as `split_exchange` but uses real USDC + CTF mocks
    (delegate/usdc_mock.py + delegate/ctf_mock.py) for collateral / ctf
    slots. proxy/safe/outcome factories stay on universal_mock — those
    slots only need getImplementation()/masterCopy() to respond during
    init. Returns (h1, h2, orch, usdc, ctf)."""
    h1, h2, orch = _build_split_exchange(
        localnet, admin,
        collateral_app_id=usdc_stateful.app_id,
        ctf_app_id=ctf_stateful.app_id,
        ctf_collateral_app_id=usdc_stateful.app_id,
        factory_app_id=universal_mock.app_id,
        # MINT/MERGE settle through outcomeTokenFactory.{splitPosition,
        # mergePositions}. Point that slot at the real CTF mock so those
        # calls reach an implementation; proxy/safe factories stay on
        # universal_mock for getImplementation()/masterCopy() during init.
        outcome_factory_app_id=ctf_stateful.app_id,
    )
    return h1, h2, orch, usdc_stateful, ctf_stateful


@pytest.fixture(scope="function")
def split_exchange_with_delegate(localnet, admin, split_exchange, lonely_chunk_artifacts):
    """Deploy a LonelyChunk sidecar that swaps orch's approval to the F-helper
    bytes, invokes orch, then reverts.

    F-helper used to be Helper2 (the manually-curated matchOrders + closure
    bundle). After `--force-delegate matchOrders` landed in `compile_all.sh`,
    puya-sol auto-emits Helper3 with matchOrders + every transitive helper
    pulled in. F = Helper3 now.

    `__self_bytes` box is populated with helper3's compiled bytes.
    """
    h1, h2, orch = split_exchange

    spec = lonely_chunk_artifacts["spec"]
    approval_bin = lonely_chunk_artifacts["approval_bin"]
    clear_bin = lonely_chunk_artifacts["clear_bin"]

    f_bytes = (H3_DIR / "CTFExchange__Helper3.approval.bin").read_bytes()

    extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
    sp = localnet.client.algod.suggested_params()
    orch_info = localnet.client.algod.application_info(orch.app_id)
    from algosdk import encoding as _enc
    orch_orig_bytes = _enc.base64.b64decode(orch_info["params"]["approval-program"])

    init_selector = bytes.fromhex("69443df7")
    create = ApplicationCreateTxn(
        sender=admin.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin, clear_program=clear_bin,
        global_schema=StateSchema(num_uints=3, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=[
            init_selector,
            orch.app_id.to_bytes(8, "big"),
            len(f_bytes).to_bytes(8, "big"),
            len(orch_orig_bytes).to_bytes(8, "big"),
        ],
    )
    txid = localnet.client.algod.send_transaction(create.sign(admin.private_key))
    res = wait_for_confirmation(localnet.client.algod, txid, 4)
    chunk_app_id = res["application-index"]

    sp2 = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp2, algod_addr_for_app(chunk_app_id), 8_000_000)
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)

    chunk_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=chunk_app_id,
        default_sender=admin.address))

    chunk_client.send.call(au.AppClientMethodCallParams(
        method="setup_boxes", args=[],
        box_references=[
            au.BoxReference(app_id=0, name=b"__self_bytes"),
            au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
        ],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)

    CHUNK = 1024

    def _write_chunks(method: str, box_name: bytes, payload: bytes):
        for offset in range(0, len(payload), CHUNK):
            slice_ = payload[offset : offset + CHUNK]
            chunk_client.send.call(au.AppClientMethodCallParams(
                method=method,
                args=[offset, slice_],
                box_references=[au.BoxReference(app_id=0, name=box_name)],
                extra_fee=au.AlgoAmount(micro_algo=10_000),
            ), send_params=AUTO_POPULATE)

    _write_chunks("set_self_chunk", b"__self_bytes", f_bytes)
    _write_chunks("set_orch_orig_chunk", b"__orch_orig_bytes", orch_orig_bytes)

    return h1, h2, orch, chunk_client


def _build_chunk_for(localnet, admin, orch, lonely_chunk_artifacts,
                     *, h1_app_id, h2_app_id):
    """Stand up a LonelyChunk sidecar pointing at `orch`. Sets up boxes
    with helper3's bytes (template-substituted with h1/h2 ids) + orch's
    original bytes. Returns the chunk AppClient."""
    spec = lonely_chunk_artifacts["spec"]
    approval_bin = lonely_chunk_artifacts["approval_bin"]
    clear_bin = lonely_chunk_artifacts["clear_bin"]
    algod = localnet.client.algod

    # Helper3's pre-built .bin still has TMPL_CTFExchange__Helper{1,2}_APP_ID
    # placeholders. Substitute in the TEAL and recompile so the inner-call
    # ApplicationID fields resolve to real app ids when helper3 runs on
    # the orch's program.
    h3_teal = (H3_DIR / "CTFExchange__Helper3.approval.teal").read_text()
    h3_teal = inject_memory_init(h3_teal)
    h3_teal = h3_teal.replace(TMPL_H1, str(h1_app_id))
    h3_teal = h3_teal.replace(TMPL_H2, str(h2_app_id))
    h3_teal = h3_teal.replace(TMPL_H3, "0")
    f_bytes = compile_teal(algod, h3_teal)

    extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
    sp = localnet.client.algod.suggested_params()
    orch_info = localnet.client.algod.application_info(orch.app_id)
    from algosdk import encoding as _enc
    orch_orig_bytes = _enc.base64.b64decode(orch_info["params"]["approval-program"])

    init_selector = bytes.fromhex("69443df7")
    create = ApplicationCreateTxn(
        sender=admin.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin, clear_program=clear_bin,
        global_schema=StateSchema(num_uints=3, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=[
            init_selector,
            orch.app_id.to_bytes(8, "big"),
            len(f_bytes).to_bytes(8, "big"),
            len(orch_orig_bytes).to_bytes(8, "big"),
        ],
    )
    txid = localnet.client.algod.send_transaction(create.sign(admin.private_key))
    res = wait_for_confirmation(localnet.client.algod, txid, 4)
    chunk_app_id = res["application-index"]

    sp2 = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp2, algod_addr_for_app(chunk_app_id), 8_000_000)
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)

    chunk_client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=chunk_app_id,
        default_sender=admin.address))

    chunk_client.send.call(au.AppClientMethodCallParams(
        method="setup_boxes", args=[],
        box_references=[
            au.BoxReference(app_id=0, name=b"__self_bytes"),
            au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
        ],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)

    CHUNK = 1024

    def _write_chunks(method: str, box_name: bytes, payload: bytes):
        for offset in range(0, len(payload), CHUNK):
            slice_ = payload[offset : offset + CHUNK]
            chunk_client.send.call(au.AppClientMethodCallParams(
                method=method,
                args=[offset, slice_],
                box_references=[au.BoxReference(app_id=0, name=box_name)],
                extra_fee=au.AlgoAmount(micro_algo=10_000),
            ), send_params=AUTO_POPULATE)

    _write_chunks("set_self_chunk", b"__self_bytes", f_bytes)
    _write_chunks("set_orch_orig_chunk", b"__orch_orig_bytes", orch_orig_bytes)
    return chunk_client


@pytest.fixture(scope="function")
def split_settled_with_delegate(
    localnet, admin, split_exchange_settled, lonely_chunk_artifacts
):
    """`split_exchange_settled` + lonely-chunk delegate. Returns
    (h1, h2, orch, usdc, ctf, chunk).

    The chunk is registered as an operator on `orch` so its inner
    matchOrders call passes the `onlyOperator` gate (the chunk's app
    address becomes msg.sender for that inner txn)."""
    h1, h2, orch, usdc, ctf = split_exchange_settled
    chunk = _build_chunk_for(localnet, admin, orch, lonely_chunk_artifacts,
                             h1_app_id=h1.app_id, h2_app_id=h2.app_id)

    # Register the chunk's REAL algorand account address as the operator —
    # that's what msg.sender resolves to inside the orch's matchOrders
    # inner-call execution. The puya-sol address convention (24 zeros +
    # itob(app_id)) is used for storage slot encoding, NOT for msg.sender
    # comparisons on inner txns.
    from dev.addrs import algod_addr_bytes_for_app
    chunk_addr32 = algod_addr_bytes_for_app(chunk.app_id)
    orch.send.call(au.AppClientMethodCallParams(
        method="addOperator",
        args=[chunk_addr32],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)

    return h1, h2, orch, usdc, ctf, chunk
