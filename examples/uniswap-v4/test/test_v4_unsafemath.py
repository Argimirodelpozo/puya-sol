"""Uniswap V4 UnsafeMath — adapted from UnsafeMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT256, Q128

@pytest.mark.localnet
def test_divRoundingUp_maxInput(helper47):
    r = helper47.send.call(au.AppClientMethodCallParams(method="UnsafeMath.divRoundingUp", args=[MAX_UINT256, MAX_UINT256]))
    assert r.abi_return == 1

@pytest.mark.localnet
def test_divRoundingUp_roundsUp(helper47):
    r = helper47.send.call(au.AppClientMethodCallParams(method="UnsafeMath.divRoundingUp", args=[11, 5]))
    assert r.abi_return == 3

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", [
    (10, 5, 2),
    (11, 5, 3),
    (1, 3, 1),
    (Q128, 3, Q128 // 3 + 1),
    (MAX_UINT256, 2, MAX_UINT256 // 2 + 1),
])
def test_fuzz_divRoundingUp(helper47, x, y, expected):
    r = helper47.send.call(au.AppClientMethodCallParams(method="UnsafeMath.divRoundingUp", args=[x, y]))
    assert r.abi_return == expected

@pytest.mark.localnet
def test_simpleMulDiv_succeeds(helper47):
    r = helper47.send.call(au.AppClientMethodCallParams(method="UnsafeMath.simpleMulDiv", args=[10, 3, 7]))
    assert r.abi_return == 4

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,d,expected", [
    (100, 200, 400, 50),
    (1, 1, 1, 1),
    (Q128, 2, 4, Q128 // 2),
])
def test_fuzz_simpleMulDiv(helper47, x, y, d, expected):
    r = helper47.send.call(au.AppClientMethodCallParams(method="UnsafeMath.simpleMulDiv", args=[x, y, d]))
    assert r.abi_return == expected
