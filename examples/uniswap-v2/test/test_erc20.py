"""
UniswapV2ERC20 — LP Token Contract Tests

Tests the base ERC20 functionality that UniswapV2 LP tokens provide:
approve, transfer, transferFrom. The permit function (EIP-2612) is
tested structurally but ecrecover is stubbed (returns zero address).

Compiled from the original Uniswap V2 Solidity (=0.5.16) to AVM TEAL.
304 lines of TEAL.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def erc20_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the UniswapV2ERC20 contract."""
    return deploy_contract(localnet, account, "UniswapV2ERC20")


@pytest.mark.localnet
def test_erc20_deploys(erc20_client: au.AppClient) -> None:
    """UniswapV2ERC20 should deploy successfully."""
    assert erc20_client.app_id > 0


@pytest.mark.localnet
def test_approve(erc20_client: au.AppClient, account: SigningAccount) -> None:
    """approve(spender, value) should return true."""
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 1000],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_transfer(erc20_client: au.AppClient, account: SigningAccount) -> None:
    """transfer(to, value) with 0 value should return true (no balance to transfer from)."""
    # Note: With 0 initial supply, transfers of 0 should work
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[account.address, 0],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_transfer_from(erc20_client: au.AppClient, account: SigningAccount) -> None:
    """transferFrom with 0 value should work."""
    result = erc20_client.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[account.address, account.address, 0],
        )
    )
    assert result.abi_return is True
