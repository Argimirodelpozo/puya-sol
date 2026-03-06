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
    """Deploy orchestrator + all helpers with group validation."""
    orch = deploy_contract(localnet, account, out_dir, prefix)

    # Count helpers
    helper_count = 0
    while (out_dir / f"{prefix}__Helper{helper_count + 1}.arc56.json").exists():
        helper_count += 1

    helpers = {}
    for i in range(1, helper_count + 1):
        name = f"{prefix}__Helper{i}"
        helpers[i] = deploy_contract(
            localnet, account, out_dir, name,
            orch_app_id=orch.app_id,
        )

    return orch, helpers


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
