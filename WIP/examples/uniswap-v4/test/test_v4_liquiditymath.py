"""Uniswap V4 LiquidityMath — adapted from LiquidityMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT128, MAX_INT128, MIN_INT128
from helpers import to_int256, grouped_call

@pytest.mark.localnet
def test_addDelta_throwsForUnderflow_case1(helper48, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "LiquidityMath.addDelta", [0, to_int256(-1)], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_addDelta_throwsForUnderflow_case2(helper48, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "LiquidityMath.addDelta", [MAX_INT128, to_int256(MIN_INT128)], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_addDelta_throwsForOverflow(helper48, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper48, "LiquidityMath.addDelta", [MAX_UINT128, to_int256(1)], orchestrator, algod_client, account)

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (1000, 500, 1500),
    (1000, -300, 700),
    (0, 100, 100),
])
def test_addDelta_valid(helper48, x, y, expected, orchestrator, algod_client, account):
    r = grouped_call(helper48, "LiquidityMath.addDelta", [x, to_int256(y)], orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (MAX_UINT128 - 1, 1, MAX_UINT128),
    (100, -99, 1),
    (1, -1, 0),
])
def test_addDelta_boundary(helper48, x, y, expected, orchestrator, algod_client, account):
    r = grouped_call(helper48, "LiquidityMath.addDelta", [x, to_int256(y)], orchestrator, algod_client, account)
    assert r == expected
