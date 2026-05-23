"""Tests for the dev/mocks ERC20 used as USDC stand-in."""
import algokit_utils as au
from conftest import AUTO_POPULATE, addr


def _call(client, method, args=None, sender=None, extra_fee=10_000):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=AUTO_POPULATE).abi_return


def test_deploy_metadata(erc20):
    assert _call(erc20, "name") == "USDC Mock"
    assert _call(erc20, "symbol") == "USDC"


def test_initial_total_supply_is_zero(erc20):
    assert _call(erc20, "totalSupply") == 0


def test_balance_of_unfunded_is_zero(erc20, funded_account):
    assert _call(erc20, "balanceOf", [addr(funded_account)]) == 0


def test_mint_increases_balance_and_supply(erc20, admin, funded_account):
    _call(erc20, "mint", [addr(funded_account), 1000], sender=admin)
    assert _call(erc20, "balanceOf", [addr(funded_account)]) == 1000
    assert _call(erc20, "totalSupply") == 1000
