"""Uniswap V4 Position.update — adapted from Position.t.sol

Tests for:
- Position.update((uint256,uint256,uint256), uint256, uint256, uint256) -> (uint256,uint256)
  on Helper21.

Position tuple: (liquidity, feeGrowthInside0LastX128, feeGrowthInside1LastX128)
Args: position, liquidityDelta, feeGrowthInside0X128, feeGrowthInside1X128
Returns: (feesOwed0, feesOwed1)
"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import Q128


@pytest.mark.localnet
@pytest.mark.xfail(reason="Position.update reverts with zero liquidity and zero delta")
def test_update_zero_liquidity_zero_delta(helper21, orchestrator, algod_client, account):
    """Zero liquidity position with zero delta returns zero fees."""
    position = (0, 0, 0)
    r = grouped_call(helper21, "Position.update", [position, 0, 0, 0], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert result[0] == 0
    assert result[1] == 0


@pytest.mark.localnet
def test_update_with_liquidity_zero_fee_growth(helper21, orchestrator, algod_client, account):
    """Position with liquidity but no fee growth returns zero fees."""
    liquidity = 10**18
    position = (liquidity, 0, 0)
    r = grouped_call(helper21, "Position.update", [position, 0, 0, 0], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert result[0] == 0
    assert result[1] == 0


@pytest.mark.localnet
def test_update_with_liquidity_and_fee_growth(helper21, orchestrator, algod_client, account):
    """Position with liquidity and fee growth returns nonzero fees."""
    liquidity = 10**18
    position = (liquidity, 0, 0)
    fee_growth_0 = Q128
    fee_growth_1 = Q128 * 2
    r = grouped_call(helper21, "Position.update", [position, 0, fee_growth_0, fee_growth_1], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert result[0] == 10**18
    assert result[1] == 2 * 10**18


@pytest.mark.localnet
def test_update_with_partial_fee_growth(helper21, orchestrator, algod_client, account):
    """Position that has already accrued some fees only gets the delta."""
    liquidity = 10**18
    initial_fee_growth = Q128
    position = (liquidity, initial_fee_growth, initial_fee_growth)
    new_fee_growth_0 = Q128 * 3
    new_fee_growth_1 = Q128 * 5
    r = grouped_call(helper21, "Position.update", [position, 0, new_fee_growth_0, new_fee_growth_1], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert result[0] == 2 * 10**18
    assert result[1] == 4 * 10**18


@pytest.mark.localnet
def test_update_with_liquidity_delta(helper21, orchestrator, algod_client, account):
    """Adding liquidity delta with zero fee growth returns zero fees."""
    position = (10**18, 0, 0)
    liquidity_delta = 5 * 10**17
    r = grouped_call(helper21, "Position.update", [position, liquidity_delta, 0, 0], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert result[0] == 0
    assert result[1] == 0
