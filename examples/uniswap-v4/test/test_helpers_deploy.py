"""
Uniswap V4 — Helper Contract Deployment Tests

Tests that all 47 helper contracts + the orchestrator deploy successfully.
"""
import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_helper


@pytest.mark.localnet
def test_deploy_orchestrator(localnet: au.AlgorandClient, account: SigningAccount) -> None:
    client = deploy_helper(localnet, account, "PoolManager")
    assert client.app_id > 0


@pytest.mark.localnet
@pytest.mark.parametrize("helper_num", range(1, 48))
def test_deploy_helper(
    localnet: au.AlgorandClient, account: SigningAccount, helper_num: int
) -> None:
    name = f"PoolManager__Helper{helper_num}"
    client = deploy_helper(localnet, account, name)
    assert client.app_id > 0
