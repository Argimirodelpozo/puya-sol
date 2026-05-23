"""Translation of v1 src/exchange/test/libraries/CalculatorHelper.t.sol.

Three pure-math tests that exercise CalculatorHelper functions through the
helper contract directly. The Foundry originals are fuzz-tests with
vm.assume; we use parametrized concrete inputs instead since AVM has no fuzz
runner native to algokit.
"""
import algokit_utils as au
import pytest
from conftest import AUTO_POPULATE


# Side enum: BUY=0, SELL=1
SIDE_BUY = 0
SIDE_SELL = 1


def _call(client, method, args, extra_fee=20_000):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=AUTO_POPULATE).abi_return


# Mirrors testFuzzCalculateTakingAmount: making * takerAmount / makerAmount.
@pytest.mark.parametrize("making,maker,taker", [
    (10, 100, 50),    # 10 * 50 / 100 = 5
    (1, 100, 200),    # 1 * 200 / 100 = 2
    (50, 100, 100),   # 50 * 100 / 100 = 50
    (99, 100, 50),    # 99 * 50 / 100 = 49 (truncating div)
])
def test_calculate_taking_amount(helper_only, making, maker, taker):
    res = _call(helper_only, "CalculatorHelper.calculateTakingAmount",
                [making, maker, taker])
    assert res == making * taker // maker


# Mirrors testFuzzCalculatePrice: just runs the function for both sides
# without expected-value asserts (the original test only checks no revert).
@pytest.mark.parametrize("maker,taker,side", [
    (100, 50, SIDE_BUY),
    (50, 100, SIDE_BUY),
    (100, 100, SIDE_SELL),
    (1, 1, SIDE_BUY),
])
def test_calculate_price_does_not_revert(helper_only, maker, taker, side):
    # No assertion on result — the original test verifies the call succeeds.
    _call(helper_only, "CalculatorHelper._calculatePrice", [maker, taker, side])


# Mirrors testFuzzIsCrossing: compute two prices, then call _isCrossing.
# Original test verifies no revert; we do same.
@pytest.mark.parametrize("makerA,takerA,sideA,makerB,takerB,sideB", [
    (100, 50, SIDE_BUY, 100, 50, SIDE_SELL),
    (50, 100, SIDE_SELL, 100, 50, SIDE_BUY),
    (10, 5, SIDE_BUY, 5, 10, SIDE_BUY),
])
def test_is_crossing_does_not_revert(helper_only,
                                      makerA, takerA, sideA,
                                      makerB, takerB, sideB):
    priceA = _call(helper_only, "CalculatorHelper._calculatePrice", [makerA, takerA, sideA])
    priceB = _call(helper_only, "CalculatorHelper._calculatePrice", [makerB, takerB, sideB])
    _call(helper_only, "CalculatorHelper._isCrossing",
          [priceA, priceB, sideA, sideB])
