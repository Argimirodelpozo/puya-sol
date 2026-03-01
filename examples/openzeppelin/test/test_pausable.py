"""OpenZeppelin Pausable behavioral tests."""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def pausable_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "PausableTest", fund_amount=0)


@pytest.mark.localnet
def test_initially_not_paused(pausable_client: au.AppClient) -> None:
    result = pausable_client.send.call(
        au.AppClientMethodCallParams(method="paused")
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_do_something_when_not_paused(pausable_client: au.AppClient) -> None:
    result = pausable_client.send.call(
        au.AppClientMethodCallParams(method="doSomething")
    )
    assert result.abi_return == 42


@pytest.mark.localnet
def test_pause(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="paused")
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_do_something_when_paused_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(method="doSomething")
        )


@pytest.mark.localnet
def test_pause_when_already_paused_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(method="pause")
        )


@pytest.mark.localnet
def test_unpause(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    client.send.call(
        au.AppClientMethodCallParams(method="unpause")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="paused")
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_unpause_when_not_paused_fails(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(method="unpause")
        )


@pytest.mark.localnet
def test_do_something_after_unpause(
    localnet: au.AlgorandClient, account: SigningAccount
) -> None:
    client = deploy_contract(localnet, account, "PausableTest", fund_amount=0)
    client.send.call(
        au.AppClientMethodCallParams(method="pause")
    )
    client.send.call(
        au.AppClientMethodCallParams(method="unpause")
    )
    result = client.send.call(
        au.AppClientMethodCallParams(method="doSomething")
    )
    assert result.abi_return == 42
