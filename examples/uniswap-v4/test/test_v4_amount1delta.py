"""Uniswap V4 SqrtPriceMath.getAmount1Delta — 2-chunk method tests.

Tests for:
- SqrtPriceMath.getAmount1Delta__chunk_0(uint256, uint256, uint256, bool)
    -> (uint256, uint256, uint256, uint256) on Helper44
- SqrtPriceMath.getAmount1Delta__chunk_1(uint256, uint256, uint256, bool,
    uint256, uint256, uint256, uint256) -> uint256 on Helper32

chunk_0 produces 4 intermediate values; chunk_1 takes the original 4 args
plus those 4 intermediates and returns the final amount1 delta.
"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import SQRT_PRICE_1_1, SQRT_PRICE_2_1, Q96


def call_getAmount1Delta(helper44, helper32, sqrtPriceA, sqrtPriceB, liquidity, roundUp,
                         orchestrator=None, algod=None, account=None):
    """Call the 2-chunk getAmount1Delta: chunk_0 on H44, chunk_1 on H32."""
    r0 = grouped_call(helper44, "SqrtPriceMath.getAmount1Delta__chunk_0", [sqrtPriceA, sqrtPriceB, liquidity, roundUp], orchestrator, algod, account)
    intermediates = r0
    assert isinstance(intermediates, (list, tuple)), "chunk_0 should return a tuple"
    assert len(intermediates) == 4, f"chunk_0 should return 4 values, got {len(intermediates)}"

    # chunk_1 takes: original 4 args + 4 intermediates
    r1 = grouped_call(helper32, "SqrtPriceMath.getAmount1Delta__chunk_1", [sqrtPriceA, sqrtPriceB, liquidity, roundUp] + list(intermediates), orchestrator, algod, account)
    return r1


@pytest.mark.localnet
def test_getAmount1Delta_same_prices(helper44, helper32, orchestrator, algod_client, account):
    """Same sqrt prices should return 0 delta."""
    result = call_getAmount1Delta(helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_1_1, 10**18, True, orchestrator=orchestrator, algod=algod_client, account=account)
    assert result == 0


@pytest.mark.localnet
def test_getAmount1Delta_roundUp(helper44, helper32, orchestrator, algod_client, account):
    """getAmount1Delta with roundUp=True between SQRT_PRICE_1_1 and SQRT_PRICE_2_1.

    amount1 = liquidity * (sqrtPriceB - sqrtPriceA) / Q96 (rounded up)
    With SQRT_PRICE_1_1 = Q96 and SQRT_PRICE_2_1 = sqrt(2)*Q96:
        diff = SQRT_PRICE_2_1 - SQRT_PRICE_1_1
        amount1 = 1e18 * diff / Q96  (rounded up)
    """
    result = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, True
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert result > 0
    # The expected value: liquidity * (sqrtB - sqrtA) / Q96, rounded up
    diff = SQRT_PRICE_2_1 - SQRT_PRICE_1_1
    expected_exact = 10**18 * diff
    expected_rounded_up = (expected_exact + Q96 - 1) // Q96
    assert result == expected_rounded_up


@pytest.mark.localnet
def test_getAmount1Delta_roundDown(helper44, helper32, orchestrator, algod_client, account):
    """getAmount1Delta with roundUp=False should be <= roundUp result."""
    result_up = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, True
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    result_down = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, False
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert result_down > 0
    assert result_down <= result_up
    # The expected value: liquidity * (sqrtB - sqrtA) / Q96, rounded down
    diff = SQRT_PRICE_2_1 - SQRT_PRICE_1_1
    expected_rounded_down = (10**18 * diff) // Q96
    assert result_down == expected_rounded_down


@pytest.mark.localnet
def test_getAmount1Delta_reversed_prices(helper44, helper32, orchestrator, algod_client, account):
    """Passing prices in reverse order should give the same result (absolute diff)."""
    result_forward = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, True
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    result_reverse = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_2_1, SQRT_PRICE_1_1, 10**18, True
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert result_forward == result_reverse


@pytest.mark.localnet
def test_getAmount1Delta_small_liquidity(helper44, helper32, orchestrator, algod_client, account):
    """Small liquidity should produce small delta."""
    result = call_getAmount1Delta(
        helper44, helper32, SQRT_PRICE_1_1, SQRT_PRICE_2_1, 1, True
    ,
        orchestrator=orchestrator, algod=algod_client, account=account)
    # With liquidity=1, result should be very small
    assert result >= 0
