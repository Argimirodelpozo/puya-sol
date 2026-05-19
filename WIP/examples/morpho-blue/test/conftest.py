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
    client.set_suggested_params_cache_timeout(0)  # disable caching for dev mode
    client.account.set_signer_from_account(account)
    return client


def load_arc56(name: str, subdir: str | None = None) -> au.Arc56Contract:
    base = OUT_DIR / (subdir or name)
    arc56_path = base / f"{name}.arc56.json"
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
    orch_app_id: int | None = None,
) -> au.AppClient:
    """Deploy a compiled contract and return an AppClient pointing at
    the user-facing app (main app for split contracts; the single app
    for non-split). For split contracts (Morpho), pass `orch_app_id`
    fixture; the dance is run transparently and the returned AppClient
    is bound to `main_id`.
    """
    app_spec = load_arc56(name, subdir)
    contract_dir = OUT_DIR / (subdir or name)
    if (contract_dir / "deploy.uros.json").exists():
        if orch_app_id is None:
            raise RuntimeError(
                f"{name} is uros-split; pass orch_app_id fixture to "
                f"deploy_contract"
            )
        from uros_dance import deploy_split_app
        d = deploy_split_app(
            localnet.client.algod, account, name,
            orch_id=orch_app_id,
            app_args=constructor_args or [],
            fund_amount=fund_amount,
        )
        return au.AppClient(
            au.AppClientParams(
                algorand=localnet,
                app_spec=app_spec,
                app_id=d.main_id,
                default_sender=account.address,
            )
        )
    client = deploy_contract_raw(
        localnet, account, name, app_spec,
        subdir=subdir,
        app_args=constructor_args or [],
        extra_pages=extra_pages,
    )
    if fund_amount > 0:
        fund_contract(localnet, account, client.app_id, fund_amount)

    # puya-sol's "constructor box-write auto-split" pattern: AppCreate
    # sets `__ctor_pending = 1`; the actual constructor body runs in
    # `__postInit()`. Mocks (ERC20Mock, IrmMock etc.) have state-var
    # initializers (e.g. `uint public totalSupply = 0`) that only land
    # after __postInit fires. Tests reading those state vars before
    # __postInit see `app_global_get_ex` assert-fail.
    postinit_method = next(
        (m for m in getattr(app_spec, "methods", []) or []
         if getattr(m, "name", None) == "__postInit"),
        None,
    )
    args_in = constructor_args or []
    if postinit_method is not None and len(args_in) >= len(postinit_method.args):
        from algosdk.atomic_transaction_composer import (
            AtomicTransactionComposer, TransactionWithSigner,
            AccountTransactionSigner,
        )
        from algosdk.transaction import ApplicationCallTxn, OnComplete
        algod = localnet.client.algod
        signer = AccountTransactionSigner(account.private_key)
        sp = algod.suggested_params()
        arg_types = ",".join(getattr(a, "type", "") for a in postinit_method.args)
        sig = f"__postInit({arg_types})void"
        sel = hashlib.new("sha512_256", sig.encode()).digest()[:4]
        call_args = [sel] + list(args_in)[: len(postinit_method.args)]
        call_txn = ApplicationCallTxn(
            sender=account.address, sp=sp, index=client.app_id,
            on_complete=OnComplete.NoOpOC, app_args=call_args,
        )
        atc = AtomicTransactionComposer()
        atc.add_transaction(TransactionWithSigner(call_txn, signer))
        try:
            atc = au.populate_app_call_resources(atc, algod)
        except Exception:
            pass
        try:
            atc.execute(algod, 4)
        except Exception:
            # __postInit may revert on mis-encoded ctor args; let the
            # downstream test surface the real failure mode (e.g.
            # field-read returning default).
            pass
    return client


@pytest.fixture(scope="module")
def orch_app_id(localnet, account):
    """Module-scoped: each test file deploys its own Uros orchestrator.
    Module isolation matters because the orch holds chunk bytecode in
    boxes keyed by chunk index. Two split contracts deployed via the
    same orch would collide on chunk_0/.../chunk_N (the second
    `Box.create` asserts on existence)."""
    from uros_dance import deploy_orchestrator
    return deploy_orchestrator(localnet.client.algod, account)


def deploy_contract_raw(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    app_spec: au.Arc56Contract,
    subdir: str | None = None,
    app_args: list[bytes] | None = None,
    extra_pages: int = 0,
) -> au.AppClient:
    algod = localnet.client.algod
    base = OUT_DIR / (subdir or name)

    approval_path = base / f"{name}.approval.teal"
    clear_path = base / f"{name}.clear.teal"
    approval_result = algod.compile(approval_path.read_text())
    clear_result = algod.compile(clear_path.read_text())
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    # Auto-detect extra pages needed (base page = 2KB, each extra = 2KB)
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


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    """Compute the box key for a Solidity mapping.

    puya-sol's per-layer-hashed (Solidity-style) derivation:
        prefix_0  = utf8(mapping_name)
        prefix_i  = sha256(key_bytes_i ++ prefix_{i-1})  for i in 1..N
        box_key   = prefix_N
    Each key must already be encoded in its canonical form (uint64→itob
    8 B; biguint/uint256→32-byte pad; bytes/bytesN/address→raw).
    """
    prefix = mapping_name.encode()
    for k in keys:
        prefix = hashlib.sha256(k + prefix).digest()
    return prefix


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def addr_to_bytes32(addr: str) -> bytes:
    """Convert Algorand address to 32-byte padded value for Solidity address type."""
    raw = encoding.decode_address(addr)
    return b"\x00" * (32 - len(raw)) + raw


def app_id_to_bytes32(app_id: int) -> bytes:
    """Convert app ID to 32-byte Solidity-style address (zero-padded uint64).

    The puya-sol compiler's inner call mechanism uses extract_uint64(addr, 24)
    to get the app ID from a 32-byte address field, so addresses must be
    encoded as 24 zero bytes + 8-byte big-endian app ID.
    """
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def app_id_to_algod_addr(app_id: int) -> str:
    """Convert app ID to Algorand address format for ABI calls.

    Returns the 58-char Algorand address encoding of the zero-padded app ID.
    """
    raw = b"\x00" * 24 + app_id.to_bytes(8, "big")
    return encoding.encode_address(raw)


def int_to_bytes32(val: int) -> bytes:
    """Convert integer to 32-byte big-endian uint256 for mapping key normalization."""
    return val.to_bytes(32, "big")


def int_to_bytes64(val: int) -> bytes:
    """Convert integer to 64-byte big-endian biguint for mapping key normalization."""
    return val.to_bytes(64, "big")
