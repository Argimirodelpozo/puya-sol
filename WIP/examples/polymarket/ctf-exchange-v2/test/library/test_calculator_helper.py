"""Translation of v2 src/test/libraries/CalculatorHelper.t.sol.

Foundry tests are fuzz-tests; we use parametrized concrete inputs (algokit
has no fuzz runner). Tests call the helper1's CalculatorHelper.* methods
directly via inner-app-call.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from conftest import AUTO_POPULATE


SIDE_BUY = 0
SIDE_SELL = 1


def _call(client, method, args, extra_fee=20_000):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=AUTO_POPULATE).abi_return


# Mirrors test_CalculatorHelper_FuzzCalculateTakingAmount:
#   takingAmount = making * takerAmount / makerAmount  (round-down)
@pytest.mark.parametrize("making,maker,taker", [
    (10, 100, 50),    # 5
    (1, 100, 200),    # 2
    (50, 100, 100),   # 50
    (99, 100, 50),    # 49 (round down)
    (1, 1, 1),        # 1
    (0, 100, 50),     # 0
])
def test_calculate_taking_amount(helper1, making, maker, taker):
    res = _call(helper1, "CalculatorHelper.calculateTakingAmount",
                [making, maker, taker])
    assert res == making * taker // maker


# test_CalculatorHelper_revert_CalculateTakingAmountOverflow: divide by zero
def test_calculate_taking_amount_revert_zero_maker(helper1):
    """making * takerAmount / 0 reverts."""
    with pytest.raises(LogicError):
        _call(helper1, "CalculatorHelper.calculateTakingAmount", [10, 0, 50])
