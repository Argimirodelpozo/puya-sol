"""
M38: Public state variable getters (Gap 9 verification).
Tests that `public` state variables automatically generate getter methods.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PublicGettersTest")


@pytest.mark.localnet
def test_threshold_getter(client: au.AppClient) -> None:
    """Auto-generated threshold() getter should return initial value."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="threshold", args=[])
    )
    assert result.abi_return == 100


@pytest.mark.localnet
def test_admin_getter(client: au.AppClient, account: SigningAccount) -> None:
    """Auto-generated admin() getter should return msg.sender from constructor."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="admin", args=[])
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_paused_getter(client: au.AppClient) -> None:
    """Auto-generated paused() getter should return false."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="paused", args=[])
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_counter_getter(client: au.AppClient) -> None:
    """Auto-generated counter() getter should return 0."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="counter", args=[])
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_set_and_get_threshold(client: au.AppClient) -> None:
    """Set threshold then read via auto-getter."""
    client.send.call(
        au.AppClientMethodCallParams(method="setThreshold", args=[999])
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="threshold", args=[])
    )
    assert result.abi_return == 999


@pytest.mark.localnet
def test_set_and_get_paused(client: au.AppClient) -> None:
    """Set paused then read via auto-getter."""
    client.send.call(
        au.AppClientMethodCallParams(method="setPaused", args=[True])
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="paused", args=[])
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_increment_and_get_counter(client: au.AppClient) -> None:
    """Increment counter then read via auto-getter."""
    client.send.call(
        au.AppClientMethodCallParams(method="increment", args=[])
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="counter", args=[])
    )
    assert result.abi_return == 1

    # Increment again (use note to avoid duplicate txn ID)
    client.send.call(
        au.AppClientMethodCallParams(method="increment", args=[], note=b"2")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="counter", args=[], note=b"r2")
    )
    assert result.abi_return == 2
