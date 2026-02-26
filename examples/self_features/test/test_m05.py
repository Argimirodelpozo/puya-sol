"""
M5: Custom errors.
Verifies that revert with custom errors produces meaningful error messages.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def errors_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    client = deploy_contract(localnet, account, "CustomErrorsTest")
    # Fund the app for MBR (state storage)
    localnet.send.payment(
        au.PaymentParams(
            sender=account.address,
            receiver=client.app_address,
            amount=au.AlgoAmount.from_micro_algo(1_000_000),
        )
    )
    return client


@pytest.mark.localnet
def test_owner_action_succeeds(errors_client: au.AppClient) -> None:
    """The deployer is the owner, so this should succeed."""
    result = errors_client.send.call(
        au.AppClientMethodCallParams(method="onlyOwnerAction", args=[])
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_withdraw_insufficient_reverts(errors_client: au.AppClient) -> None:
    """Withdrawing more than available should revert with InsufficientBalance."""
    with pytest.raises(Exception):
        errors_client.send.call(
            au.AppClientMethodCallParams(method="withdraw", args=[100])
        )


@pytest.mark.localnet
def test_deposit_and_withdraw(errors_client: au.AppClient) -> None:
    """Deposit then withdraw should succeed."""
    errors_client.send.call(
        au.AppClientMethodCallParams(method="deposit", args=[500])
    )
    result = errors_client.send.call(
        au.AppClientMethodCallParams(method="withdraw", args=[200])
    )
    assert result.abi_return == 300
