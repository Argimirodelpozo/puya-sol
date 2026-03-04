"""Uniswap V4 LiquidityMath — adapted from LiquidityMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT128, MAX_INT128, MIN_INT128
from helpers import to_int256

@pytest.mark.localnet
def test_addDelta_throwsForUnderflow_case1(helper46):
    with pytest.raises(Exception):
        helper46.send.call(au.AppClientMethodCallParams(method="LiquidityMath.addDelta", args=[0, to_int256(-1)]))

@pytest.mark.localnet
def test_addDelta_throwsForUnderflow_case2(helper46):
    with pytest.raises(Exception):
        helper46.send.call(au.AppClientMethodCallParams(method="LiquidityMath.addDelta", args=[MAX_INT128, to_int256(MIN_INT128)]))

@pytest.mark.localnet
def test_addDelta_throwsForOverflow(helper46):
    with pytest.raises(Exception):
        helper46.send.call(au.AppClientMethodCallParams(method="LiquidityMath.addDelta", args=[MAX_UINT128, to_int256(1)]))

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (1000, 500, 1500),
    (1000, -300, 700),
    (0, 100, 100),
])
def test_addDelta_valid(helper46, x, y, expected):
    r = helper46.send.call(au.AppClientMethodCallParams(method="LiquidityMath.addDelta", args=[x, to_int256(y)]))
    assert r.abi_return == expected

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (MAX_UINT128 - 1, 1, MAX_UINT128),
    (100, -99, 1),
    (1, -1, 0),
])
def test_addDelta_boundary(helper46, x, y, expected):
    r = helper46.send.call(au.AppClientMethodCallParams(method="LiquidityMath.addDelta", args=[x, to_int256(y)]))
    assert r.abi_return == expected
