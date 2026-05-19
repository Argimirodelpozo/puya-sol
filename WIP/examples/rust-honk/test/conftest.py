"""
rust-honk — Test Configuration

Deploys HonkVerifier (plain) and ZKHonkVerifier with all helpers.
Uses group validation for orchestrator + helper chain.
"""
from pathlib import Path

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
from algokit_utils.models.account import SigningAccount
import pytest

from helpers import deploy_contract

# Output directories
PLAIN_OUT_DIR = Path(__file__).parent.parent / "out" / "HonkVerifier"
ZK_OUT_DIR = Path(__file__).parent.parent / "out" / "ZKHonkVerifier"


# --- Session fixtures ---

@pytest.fixture(scope="session")
def algod_client() -> AlgodClient:
    config = au.ClientManager.get_default_localnet_config("algod")
    return au.ClientManager.get_algod_client(config)


@pytest.fixture(scope="session")
def kmd_client() -> KMDClient:
    config = au.ClientManager.get_default_localnet_config("kmd")
    return au.ClientManager.get_kmd_client(config)


@pytest.fixture(scope="session")
def localnet_clients(algod_client, kmd_client) -> au.AlgoSdkClients:
    return au.AlgoSdkClients(algod=algod_client, kmd=kmd_client)


@pytest.fixture(scope="session")
def account(localnet_clients) -> SigningAccount:
    return au.AlgorandClient(localnet_clients).account.localnet_dispenser()


@pytest.fixture(scope="session")
def localnet(localnet_clients, account) -> au.AlgorandClient:
    client = au.AlgorandClient(localnet_clients)
    client.account.set_signer_from_account(account)
    return client


# --- Deploy all contracts for a verifier ---

def deploy_verifier_suite(localnet, account, out_dir, prefix):
    """Deploy main orchestrator + all pure helpers.

    Pure helpers are sidecar contracts emitted by puya-sol's
    `--deploy-pure-helpers` flag. The manifest at
    `<out_dir>/pure_helpers.json` lists each helper's contract name
    and the TMPL_<name>_APP_ID template var that the main TEAL uses
    to inner-call it. We deploy helpers first, then substitute their
    app IDs into the main TEAL before compiling/deploying main.
    """
    import json
    import re
    from helpers import deploy_raw, fund_contract, compute_extra_pages

    manifest_path = out_dir / "pure_helpers.json"
    pure_helpers = []
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        pure_helpers = manifest.get("helpers", [])

    # Deploy each pure helper in dependency order. A helper can only
    # be deployed once all the helpers it inner-calls have been
    # deployed (their TMPL vars resolved). Iterate until all done.
    helper_app_ids = {}  # template_var → app_id
    helpers = {}         # name → AppClient
    remaining = list(pure_helpers)
    while remaining:
        progress = False
        next_round = []
        for h in remaining:
            name = h["contract_name"]
            teal_src = (out_dir / f"{name}.approval.teal").read_text()
            # Find unresolved TMPL_PURE_HELPER_* vars referenced.
            unresolved = []
            for h2 in pure_helpers:
                tmpl = f"TMPL_{h2['template_var']}"
                if tmpl in teal_src and h2["template_var"] not in helper_app_ids:
                    unresolved.append(tmpl)
            if unresolved:
                next_round.append(h)
                continue
            # All deps resolved; deploy this helper.
            client = _deploy_with_substitutions(
                localnet, account, out_dir, name, helper_app_ids)
            helper_app_ids[h["template_var"]] = client.app_id
            helpers[name] = client
            progress = True
        if not progress and next_round:
            unresolved_names = [h["contract_name"] for h in next_round]
            raise RuntimeError(
                f"Pure helpers form a cycle or have unsatisfiable deps: {unresolved_names}")
        remaining = next_round

    # Deploy main orchestrator with helpers substituted in.
    orch = _deploy_with_substitutions(
        localnet, account, out_dir, prefix, helper_app_ids)

    return orch, helpers


def _deploy_with_substitutions(localnet, account, out_dir, name, helper_app_ids):
    """Compile + deploy a contract, substituting TMPL_PURE_HELPER_*_APP_ID
    template vars in its TEAL with their resolved app IDs. Returns the
    deployed AppClient (with arc56 if present) or a thin namespace with
    `.app_id` for pure-helper sidecars (which don't have arc56)."""
    from helpers import fund_contract
    from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema, wait_for_confirmation
    from algosdk import encoding
    algod = localnet.client.algod
    approval_path = out_dir / f"{name}.approval.teal"
    clear_path = out_dir / f"{name}.clear.teal"
    approval_src = approval_path.read_text()
    clear_src = clear_path.read_text()
    # Substitute TMPL_<var> with their app_ids (the manifest's
    # template_var field already includes the _APP_ID suffix).
    for var, app_id in helper_app_ids.items():
        approval_src = approval_src.replace(f"TMPL_{var}", str(app_id))
        clear_src = clear_src.replace(f"TMPL_{var}", str(app_id))
    approval_result = algod.compile(approval_src)
    clear_result = algod.compile(clear_src)
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])
    extra_pages = max(0, (len(approval_program) - 1) // 2048)
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address, sp=sp, on_complete=OnComplete.NoOpOC,
        approval_program=approval_program, clear_program=clear_program,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]
    fund_contract(localnet, account, app_id, 1_000_000)
    arc56_path = out_dir / f"{name}.arc56.json"
    if arc56_path.exists():
        from helpers import load_arc56
        app_spec = load_arc56(out_dir, name)
        return au.AppClient(
            au.AppClientParams(
                algorand=localnet, app_spec=app_spec,
                app_id=app_id, default_sender=account.address,
            )
        )
    # Pure-helper sidecar — no arc56, just return a stub with app_id.
    class _HelperStub:
        def __init__(self, _app_id):
            self.app_id = _app_id
    return _HelperStub(app_id)


# --- Plain HonkVerifier fixtures ---

@pytest.fixture(scope="module")
def plain_verifier(localnet, account):
    """Deploy HonkVerifier (plain) orchestrator + helpers."""
    return deploy_verifier_suite(localnet, account, PLAIN_OUT_DIR, "HonkVerifier")


@pytest.fixture(scope="module")
def plain_orchestrator(plain_verifier):
    return plain_verifier[0]


@pytest.fixture(scope="module")
def plain_helpers(plain_verifier):
    return plain_verifier[1]


# --- ZK HonkVerifier fixtures ---

@pytest.fixture(scope="module")
def zk_verifier(localnet, account):
    """Deploy ZKHonkVerifier orchestrator + helpers."""
    return deploy_verifier_suite(localnet, account, ZK_OUT_DIR, "HonkVerifier")


@pytest.fixture(scope="module")
def zk_orchestrator(zk_verifier):
    return zk_verifier[0]


@pytest.fixture(scope="module")
def zk_helpers(zk_verifier):
    return zk_verifier[1]
