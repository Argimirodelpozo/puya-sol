"""OpenZeppelin ReentrancyGuard behavioral tests.

Uses exact OZ ReentrancyGuard v5.0.0 source with a test wrapper contract.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def guard_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "ReentrancyGuardTest", fund_amount=0)


@pytest.mark.localnet
def test_deploys(guard_client: au.AppClient) -> None:
    assert guard_client.app_id > 0


@pytest.mark.localnet
def test_initial_counter(guard_client: au.AppClient) -> None:
    result = guard_client.send.call(
        au.AppClientMethodCallParams(method="getCounter")
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_increment(guard_client: au.AppClient) -> None:
    guard_client.send.call(
        au.AppClientMethodCallParams(method="increment")
    )
    result = guard_client.send.call(
        au.AppClientMethodCallParams(method="getCounter")
    )
    assert result.abi_return == 1


@pytest.mark.localnet
def test_increment_multiple(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "ReentrancyGuardTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="increment", note=b"1")
    )
    client.send.call(
        au.AppClientMethodCallParams(method="increment", note=b"2")
    )
    client.send.call(
        au.AppClientMethodCallParams(method="increment", note=b"3")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="getCounter")
    )
    assert result.abi_return == 3
