"""
UniswapV2Factory — Pair Factory Contract Tests

Tests the factory contract that manages pair creation and protocol fees.
The createPair function uses EVM-specific create2 (stubbed on AVM),
but administrative functions (setFeeTo, setFeeToSetter, allPairsLength)
work natively.

Compiled from the original Uniswap V2 Solidity (=0.5.16) to AVM TEAL.
203 lines of TEAL.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def factory_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy the UniswapV2Factory contract with feeToSetter = deployer."""
    # Constructor requires _feeToSetter address as ApplicationArgs[0]
    return deploy_contract(
        localnet, account, "UniswapV2Factory",
        app_args=[encoding.decode_address(account.address)],
    )


def test_factory_deploys(factory_client: au.AppClient) -> None:
    """UniswapV2Factory should deploy successfully."""
    assert factory_client.app_id > 0


def test_all_pairs_length(factory_client: au.AppClient) -> None:
    """allPairsLength should return 0 initially."""
    result = factory_client.send.call(
        au.AppClientMethodCallParams(
            method="allPairsLength",
        )
    )
    assert result.abi_return == 0


def test_set_fee_to(factory_client: au.AppClient, account: SigningAccount) -> None:
    """setFeeTo should work when called by feeToSetter."""
    factory_client.send.call(
        au.AppClientMethodCallParams(
            method="setFeeTo",
            args=[account.address],
        )
    )


def test_set_fee_to_setter(factory_client: au.AppClient, account: SigningAccount) -> None:
    """setFeeToSetter should work when called by current feeToSetter."""
    factory_client.send.call(
        au.AppClientMethodCallParams(
            method="setFeeToSetter",
            args=[account.address],
        )
    )
