"""
M7: Nested mappings.
Verifies mapping(uint256 => mapping(uint256 => bool)) works end-to-end.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def map_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    client = deploy_contract(localnet, account, "NestedMapTest")
    # Fund the app for MBR (box storage for mappings)
    localnet.send.payment(
        au.PaymentParams(
            sender=account.address,
            receiver=client.app_address,
            amount=au.AlgoAmount.from_micro_algo(1_000_000),
        )
    )
    return client


@pytest.mark.localnet
def test_set_and_check(map_client: au.AppClient) -> None:
    """Set a nullifier and check it returns true."""
    scope, nid = 1, 42
    map_client.send.call(
        au.AppClientMethodCallParams(method="setNullifier", args=[scope, nid])
    )
    result = map_client.send.call(
        au.AppClientMethodCallParams(method="isNullified", args=[scope, nid])
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_different_key_returns_false(map_client: au.AppClient) -> None:
    """A different key should return the default (false)."""
    result = map_client.send.call(
        au.AppClientMethodCallParams(method="isNullified", args=[999, 999])
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_clear_nullifier(map_client: au.AppClient) -> None:
    """Clear a previously set nullifier."""
    scope, nid = 2, 10
    map_client.send.call(
        au.AppClientMethodCallParams(method="setNullifier", args=[scope, nid])
    )
    map_client.send.call(
        au.AppClientMethodCallParams(method="clearNullifier", args=[scope, nid])
    )
    result = map_client.send.call(
        au.AppClientMethodCallParams(method="isNullified", args=[scope, nid])
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_independent_scopes(map_client: au.AppClient) -> None:
    """Different scopes should be independent."""
    map_client.send.call(
        au.AppClientMethodCallParams(method="setNullifier", args=[10, 1])
    )
    # Same id, different scope — should be false
    result = map_client.send.call(
        au.AppClientMethodCallParams(method="isNullified", args=[20, 1])
    )
    assert result.abi_return is False
