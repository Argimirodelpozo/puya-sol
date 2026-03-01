"""
OpenZeppelin Ownable behavioral tests.

Tests the compiled Ownable contract for semantic correctness on AVM,
including: deployment, ownership queries, ownership transfer, renounce,
and access control.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

ZERO_ADDR = encoding.encode_address(b"\x00" * 32)


@pytest.fixture(scope="module")
def ownable_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy Ownable contract. Constructor sets msg.sender as owner."""
    return deploy_contract(localnet, account, "OwnableTest", fund_amount=0)


# --- Deployment tests ---


@pytest.mark.localnet
def test_deploys(ownable_client: au.AppClient) -> None:
    """Contract should deploy successfully."""
    assert ownable_client.app_id > 0


@pytest.mark.localnet
def test_owner_is_deployer(
    ownable_client: au.AppClient, account: SigningAccount
) -> None:
    """owner() should return the deployer's address."""
    result = ownable_client.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


# --- Transfer ownership tests ---


@pytest.mark.localnet
def test_transfer_ownership(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """transferOwnership should change the owner."""
    client = deploy_contract(localnet, account, "OwnableTest", fund_amount=0)

    # Transfer to a known address (just use a deterministic one)
    new_owner_raw = b"\x01" + b"\x00" * 31
    new_owner = encoding.encode_address(new_owner_raw)

    client.send.call(
        au.AppClientMethodCallParams(
            method="transferOwnership",
            args=[new_owner],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == new_owner


@pytest.mark.localnet
def test_transfer_ownership_to_zero_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """transferOwnership to address(0) should revert (OwnableInvalidOwner)."""
    client = deploy_contract(localnet, account, "OwnableTest", fund_amount=0)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="transferOwnership",
                args=[ZERO_ADDR],
            )
        )


# --- Renounce ownership tests ---


@pytest.mark.localnet
def test_renounce_ownership(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """renounceOwnership should set owner to address(0)."""
    client = deploy_contract(localnet, account, "OwnableTest", fund_amount=0)

    client.send.call(
        au.AppClientMethodCallParams(method="renounceOwnership")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == ZERO_ADDR


@pytest.mark.localnet
def test_renounce_then_transfer_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """After renouncing, transferOwnership should fail (not owner)."""
    client = deploy_contract(localnet, account, "OwnableTest", fund_amount=0)

    client.send.call(
        au.AppClientMethodCallParams(method="renounceOwnership")
    )
    new_owner_raw = b"\x01" + b"\x00" * 31
    new_owner = encoding.encode_address(new_owner_raw)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="transferOwnership",
                args=[new_owner],
            )
        )


@pytest.mark.localnet
def test_renounce_then_renounce_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    """After renouncing, renounceOwnership again should fail."""
    client = deploy_contract(localnet, account, "OwnableTest", fund_amount=0)

    client.send.call(
        au.AppClientMethodCallParams(method="renounceOwnership")
    )
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(method="renounceOwnership")
        )
