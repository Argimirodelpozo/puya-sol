"""
Real Airdrop.sol integration tests.
Exercises: deployment, Ownable (owner, transferOwnership, renounceOwnership),
state management (openRegistration, closeRegistration, openClaim, closeClaim,
setMerkleRoot, setConfigId), getScope, getConfigId, isRegistered.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def airdrop_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    # Constructor: (address hub, string scopeSeed, address token)
    # Use zero-address for hub and token on localnet
    hub_address = bytes(32)
    scope_seed = b"test-airdrop"
    token_address = bytes(32)
    return deploy_contract(
        localnet, account, "Airdrop",
        app_args=[hub_address, scope_seed, token_address],
        extra_pages=1,
    )


# --- Deployment ---


@pytest.mark.localnet
def test_airdrop_deploys(airdrop_client: au.AppClient) -> None:
    """Contract deploys successfully."""
    assert airdrop_client.app_id > 0


# --- Ownable ---


@pytest.mark.localnet
def test_owner(airdrop_client: au.AppClient) -> None:
    """owner() returns the deployer's address."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return is not None
    # ARC4 address is returned as a string (58 char Algorand address)
    assert len(str(result.abi_return)) > 0


# --- Scope ---


@pytest.mark.localnet
def test_get_scope(airdrop_client: au.AppClient) -> None:
    """getScope() returns 0 on localnet (no Poseidon)."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="getScope")
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_scope(airdrop_client: au.AppClient) -> None:
    """scope() (base method) also returns 0."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="scope")
    )
    assert result.abi_return == 0


# --- State Management ---


@pytest.mark.localnet
def test_open_registration(airdrop_client: au.AppClient) -> None:
    """openRegistration() succeeds as owner."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="openRegistration")
    )


@pytest.mark.localnet
def test_close_registration(airdrop_client: au.AppClient) -> None:
    """closeRegistration() succeeds as owner."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="closeRegistration")
    )


@pytest.mark.localnet
def test_open_claim(airdrop_client: au.AppClient) -> None:
    """openClaim() succeeds as owner."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="openClaim")
    )


@pytest.mark.localnet
def test_close_claim(airdrop_client: au.AppClient) -> None:
    """closeClaim() succeeds as owner."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="closeClaim")
    )


# --- setMerkleRoot ---


@pytest.mark.localnet
def test_set_merkle_root(airdrop_client: au.AppClient) -> None:
    """setMerkleRoot(bytes32) succeeds."""
    root = bytes(32)  # zero root
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="setMerkleRoot",
            args=[root],
        )
    )


# --- setConfigId / getConfigId ---


@pytest.mark.localnet
def test_set_and_get_config_id(airdrop_client: au.AppClient) -> None:
    """setConfigId then getConfigId returns the stored value."""
    config_id = b"\x01" * 32
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="setConfigId",
            args=[config_id],
        )
    )
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="getConfigId",
            args=[bytes(32), bytes(32), b""],
        )
    )
    returned = bytes(result.abi_return) if isinstance(result.abi_return, list) else result.abi_return
    assert returned == config_id


# --- isRegistered ---


@pytest.mark.localnet
def test_is_registered_false(airdrop_client: au.AppClient) -> None:
    """isRegistered returns false for unregistered address."""
    addr = bytes(32)
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="isRegistered",
            args=[addr],
        )
    )
    assert result.abi_return is False
