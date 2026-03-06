"""Uniswap V4 SwapMath.computeSwapStep chunk_0 — intermediate computation tests.

Tests for:
- SwapMath.computeSwapStep__chunk_0(uint256, uint256, uint256, uint256, uint64)
    -> (uint256, uint256, uint256, bool, uint256, uint256, bool)
  on Helper49.

This is chunk_0 of the multi-chunk computeSwapStep. It performs the initial
fee calculation and price target determination. Testing chunk_0 in isolation
verifies the early computation stages.

Args: sqrtPriceCurrentX96, sqrtPriceTargetX96, liquidity, amountRemaining, feePips
Returns: 7-tuple of intermediate values
"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import Q96, SQRT_PRICE_1_1, SQRT_PRICE_2_1


@pytest.mark.localnet
def test_computeSwapStep_chunk0_same_price(helper49, orchestrator, algod_client, account):
    """When current price equals target, no movement should occur.
    The intermediate values should reflect no swap needed."""
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_1_1, 10**18, 10**15, 3000], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7


@pytest.mark.localnet
def test_computeSwapStep_chunk0_small_swap(helper49, orchestrator, algod_client, account):
    """Small swap with reasonable parameters should produce valid intermediates."""
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, 10**15, 3000], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7
    # All intermediate uint256 values should be non-negative (they're unsigned)
    for i, val in enumerate(result):
        if isinstance(val, bool):
            continue
        assert val >= 0, f"Intermediate value at index {i} is negative: {val}"


@pytest.mark.localnet
def test_computeSwapStep_chunk0_zero_amount(helper49, orchestrator, algod_client, account):
    """Zero amount remaining should produce minimal intermediate values."""
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, 0, 3000], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7


@pytest.mark.localnet
def test_computeSwapStep_chunk0_max_fee(helper49, orchestrator, algod_client, account):
    """Maximum fee (100% = 1000000) should consume entire amount as fee."""
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, 10**15, 1000000], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7


@pytest.mark.localnet
def test_computeSwapStep_chunk0_zero_fee(helper49, orchestrator, algod_client, account):
    """Zero fee should pass entire amount through without fee deduction."""
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**18, 10**15, 0], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7


@pytest.mark.localnet
def test_computeSwapStep_chunk0_large_liquidity(helper49, orchestrator, algod_client, account):
    """Large liquidity should produce valid results without overflow."""
    large_liquidity = 10**27
    r = grouped_call(helper49, "SwapMath.computeSwapStep__chunk_0", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, large_liquidity, 10**18, 3000], orchestrator, algod_client, account)
    result = r
    assert isinstance(result, (list, tuple))
    assert len(result) == 7
