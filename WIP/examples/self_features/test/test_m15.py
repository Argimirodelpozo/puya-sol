"""
M15: Constructor parameters tests.
Exercises: reading constructor args from ApplicationArgs during create,
address and uint256 params, storing in state, reading back via getters.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def ctor_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    # Constructor takes (address _owner, uint256 _threshold)
    # address = 32 raw bytes of the Algorand address
    owner_bytes = encoding.decode_address(account.address)
    threshold_bytes = (500).to_bytes(32, "big")
    client = deploy_contract(
        localnet, account, "ConstructorParamsTest",
        app_args=[owner_bytes, threshold_bytes],
    )
    return client


# --- Owner tests ---


@pytest.mark.localnet
def test_get_owner(ctor_client: au.AppClient, account: SigningAccount) -> None:
    """Owner should match the address passed to constructor."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(method="getOwner")
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_is_owner_true(ctor_client: au.AppClient, account: SigningAccount) -> None:
    """isOwner returns true for the constructor-provided owner."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(
            method="isOwner",
            args=[account.address],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_is_owner_false(ctor_client: au.AppClient) -> None:
    """isOwner returns false for a different address."""
    zero_addr = encoding.encode_address(b"\x00" * 32)
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(
            method="isOwner",
            args=[zero_addr],
        )
    )
    assert result.abi_return is False


# --- Threshold tests ---


@pytest.mark.localnet
def test_get_threshold(ctor_client: au.AppClient) -> None:
    """Threshold should match the value passed to constructor."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(method="getThreshold")
    )
    assert result.abi_return == 500


@pytest.mark.localnet
def test_above_threshold_true(ctor_client: au.AppClient) -> None:
    """isAboveThreshold returns true for value > 500."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(
            method="isAboveThreshold",
            args=[501],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_above_threshold_false(ctor_client: au.AppClient) -> None:
    """isAboveThreshold returns false for value <= 500."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(
            method="isAboveThreshold",
            args=[500],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_above_threshold_zero(ctor_client: au.AppClient) -> None:
    """isAboveThreshold returns false for 0."""
    result = ctor_client.send.call(
        au.AppClientMethodCallParams(
            method="isAboveThreshold",
            args=[0],
        )
    )
    assert result.abi_return is False
