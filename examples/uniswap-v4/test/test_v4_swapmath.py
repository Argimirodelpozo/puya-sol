"""Uniswap V4 SwapMath — adapted from SwapMath.t.sol"""
import pytest
import algokit_utils as au
from constants import SQRT_PRICE_1_1, MAX_UINT160
from helpers import to_int256, grouped_call

@pytest.mark.localnet
@pytest.mark.parametrize("zeroForOne,sqrtPriceNextX96,sqrtPriceLimitX96,expected", [
    (True, 200, 100, 200),   # zeroForOne=true, next > limit → returns max(next, limit) = next
    (False, 100, 200, 100),  # zeroForOne=false, limit > next → returns min(next, limit) = next
    (True, 100, 200, 200),   # zeroForOne=true, limit > next → returns max(next, limit) = limit
    (False, 200, 100, 100),  # zeroForOne=false, next > limit → returns min(next, limit) = limit
])
def test_getSqrtPriceTarget(helper38, zeroForOne, sqrtPriceNextX96, sqrtPriceLimitX96, expected, orchestrator, algod_client, account):
    r = grouped_call(helper38, "SwapMath.getSqrtPriceTarget", [zeroForOne, sqrtPriceNextX96, sqrtPriceLimitX96], orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
def test_getSqrtPriceTarget_zeroForOne_equal(helper38, orchestrator, algod_client, account):
    """When limit equals next, returns either (they're the same)."""
    r = grouped_call(helper38, "SwapMath.getSqrtPriceTarget", [True, 100, 100], orchestrator, algod_client, account)
    assert r == 100
