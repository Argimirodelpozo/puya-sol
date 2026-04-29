from pathlib import Path

import algokit_utils as au
from algosdk.v2client.algod import AlgodClient
from algosdk.kmd import KMDClient
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


def deploy_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
) -> au.AppClient:
    app_spec = load_arc56(name)
    factory = au.AppFactory(
        au.AppFactoryParams(
            algorand=localnet,
            app_spec=app_spec,
            default_sender=account.address,
        )
    )
    client, _txn = factory.send.bare.create()
    return client
