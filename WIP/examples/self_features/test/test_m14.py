"""
M14: Airdrop-pattern tests.
Exercises: modifiers (onlyOwner), msg.sender, events with indexed params,
address-to-uint cast (uint256(uint160(addr))), mapping(uint256 => bool),
mapping(uint256 => uint256), custom errors, multiple state vars,
keccak256(abi.encodePacked(uint256, address, uint256)), cross-library calls.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount
from algosdk.v2client.algod import AlgodClient

from conftest import deploy_contract


def to_bytes(val) -> bytes:
    if isinstance(val, (bytes, bytearray)):
        return bytes(val)
    if isinstance(val, list):
        return bytes(val)
    return val


@pytest.fixture(scope="module")
def airdrop_client(
    localnet: au.AlgorandClient, account: SigningAccount, algod_client: AlgodClient
) -> au.AppClient:
    client = deploy_contract(localnet, account, "AirdropPatternsTest")
    # Fund the contract for MBR (box storage for mappings)
    au.transfer(
        algod_client,
        au.TransferParameters(
            from_account=account,
            to_address=client.app_address,
            micro_algos=1_000_000,
        ),
    )
    return client


# --- Owner tests ---


@pytest.mark.localnet
def test_get_owner(airdrop_client: au.AppClient, account: SigningAccount) -> None:
    """Owner should be set to deployer (msg.sender in constructor)."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="getOwner")
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_get_sender(airdrop_client: au.AppClient, account: SigningAccount) -> None:
    """msg.sender should return the caller's address."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="getSender")
    )
    assert result.abi_return == account.address


# --- Owner-only state management ---


@pytest.mark.localnet
def test_claim_lifecycle(airdrop_client: au.AppClient) -> None:
    """Owner can open and close claim phase."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="openClaim")
    )
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="closeClaim")
    )


# --- Config ID ---


@pytest.mark.localnet
def test_set_and_get_config_id(airdrop_client: au.AppClient) -> None:
    """Set and retrieve verificationConfigId."""
    config_id = (42).to_bytes(32, "big")
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="setConfigId",
            args=[config_id],
        )
    )
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(method="getConfigId")
    )
    assert to_bytes(result.abi_return) == config_id


# --- Merkle root ---


@pytest.mark.localnet
def test_set_merkle_root(airdrop_client: au.AppClient) -> None:
    """Set merkle root (owner-only)."""
    root = (12345).to_bytes(32, "big")
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="setMerkleRoot",
            args=[root],
        )
    )


# --- Registration with mappings ---


@pytest.mark.localnet
def test_registration_and_register(airdrop_client: au.AppClient) -> None:
    """Open registration, register a user, then close registration."""
    # Open
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="openRegistration")
    )
    # Register user
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="register",
            args=[100, 200],
        )
    )
    # Close
    airdrop_client.send.call(
        au.AppClientMethodCallParams(method="closeRegistration")
    )


# --- Address registration (uint256(uint160(address)) pattern) ---


@pytest.mark.localnet
def test_register_and_check_address(
    airdrop_client: au.AppClient, account: SigningAccount
) -> None:
    """registerAddress + isAddressRegistered tests address-to-uint cast."""
    airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="registerAddress",
            args=[account.address],
        )
    )
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="isAddressRegistered",
            args=[account.address],
        )
    )
    assert result.abi_return is True


# --- Compute claim node (keccak256 + abi.encodePacked) ---


@pytest.mark.localnet
def test_compute_claim_node(
    airdrop_client: au.AppClient, account: SigningAccount
) -> None:
    """keccak256(abi.encodePacked(index, account, amount)) produces 32-byte hash."""
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="computeClaimNode",
            args=[1, account.address, 1000],
        )
    )
    assert len(to_bytes(result.abi_return)) == 32


# --- Cross-library integration ---


@pytest.mark.localnet
def test_issuing_state(airdrop_client: au.AppClient) -> None:
    """Test getIssuingState through the Airdrop pattern contract."""
    charcodes = bytearray(93)
    charcodes[2] = ord("U")
    charcodes[3] = ord("S")
    charcodes[4] = ord("A")
    result = airdrop_client.send.call(
        au.AppClientMethodCallParams(
            method="testIssuingState",
            args=[bytes(charcodes)],
        )
    )
    assert result.abi_return == "USA"
