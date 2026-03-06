"""Uniswap V4 UnsafeMath — adapted from UnsafeMath.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import MAX_UINT256, Q128

@pytest.mark.localnet
def test_divRoundingUp_maxInput(helper49, orchestrator, algod_client, account):
    r = grouped_call(helper49, "UnsafeMath.divRoundingUp", [MAX_UINT256, MAX_UINT256], orchestrator, algod_client, account)
    assert r == 1

@pytest.mark.localnet
def test_divRoundingUp_roundsUp(helper49, orchestrator, algod_client, account):
    r = grouped_call(helper49, "UnsafeMath.divRoundingUp", [11, 5], orchestrator, algod_client, account)
    assert r == 3

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (10, 5, 2),
    (11, 5, 3),
    (1, 3, 1),
    (Q128, 3, Q128 // 3 + 1),
    (MAX_UINT256, 2, MAX_UINT256 // 2 + 1),
])
def test_fuzz_divRoundingUp(helper49, x, y, expected, orchestrator, algod_client, account):
    r = grouped_call(helper49, "UnsafeMath.divRoundingUp", [x, y], orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
def test_simpleMulDiv_succeeds(helper50, orchestrator, algod_client, account):
    r = grouped_call(helper50, "UnsafeMath.simpleMulDiv", [10, 3, 7], orchestrator, algod_client, account)
    assert r == 4

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,d,expected", [
    (100, 200, 400, 50),
    (1, 1, 1, 1),
    (Q128, 2, 4, Q128 // 2),
])
def test_fuzz_simpleMulDiv(helper50, x, y, d, expected, orchestrator, algod_client, account):
    r = grouped_call(helper50, "UnsafeMath.simpleMulDiv", [x, y, d], orchestrator, algod_client, account)
    assert r == expected
