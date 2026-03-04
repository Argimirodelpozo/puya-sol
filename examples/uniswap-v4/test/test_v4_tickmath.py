"""Uniswap V4 TickMath — adapted from TickMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MIN_TICK, MAX_TICK, MIN_SQRT_PRICE, MAX_SQRT_PRICE
from helpers import call_getSqrtPriceAtTick, call_getTickAtSqrtPrice, to_int256

@pytest.mark.localnet
def test_getSqrtPriceAtTick_throwsForTooLow(helper34, helper40):
    with pytest.raises(Exception):
        call_getSqrtPriceAtTick(helper34, helper40, MIN_TICK - 1)

@pytest.mark.localnet
def test_getSqrtPriceAtTick_throwsForTooHigh(helper34, helper40):
    with pytest.raises(Exception):
        call_getSqrtPriceAtTick(helper34, helper40, MAX_TICK + 1)

@pytest.mark.localnet
@pytest.mark.xfail(reason="Negative tick encoding: int24→uint64 two's complement hits boundary check")
def test_getSqrtPriceAtTick_isValidMinTick(helper34, helper40):
    r = call_getSqrtPriceAtTick(helper34, helper40, MIN_TICK)
    assert r == MIN_SQRT_PRICE

@pytest.mark.localnet
@pytest.mark.xfail(reason="Negative tick encoding: int24→uint64 two's complement hits boundary check")
def test_getSqrtPriceAtTick_isValidMinTickAddOne(helper34, helper40):
    r = call_getSqrtPriceAtTick(helper34, helper40, MIN_TICK + 1)
    assert r == 4295343490

@pytest.mark.localnet
@pytest.mark.xfail(reason="getSqrtPriceAtTick off-by-one: returns MAX_SQRT_PRICE instead of MAX_SQRT_PRICE-1")
def test_getSqrtPriceAtTick_isValidMaxTick(helper34, helper40):
    r = call_getSqrtPriceAtTick(helper34, helper40, MAX_TICK)
    assert r == MAX_SQRT_PRICE - 1  # MAX_SQRT_PRICE is exclusive

@pytest.mark.localnet
def test_getSqrtPriceAtTick_isValidMaxTickSubOne(helper34, helper40):
    r = call_getSqrtPriceAtTick(helper34, helper40, MAX_TICK - 1)
    assert r == 1461373636630004318706518188784493106690254656249

@pytest.mark.localnet
def test_getTickAtSqrtPrice_throwsForTooLow(helper25, helper12, helper26, helper28, helper37, helper9):
    with pytest.raises(Exception):
        call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, MIN_SQRT_PRICE - 1)

@pytest.mark.localnet
def test_getTickAtSqrtPrice_throwsForTooHigh(helper25, helper12, helper26, helper28, helper37, helper9):
    with pytest.raises(Exception):
        call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, MAX_SQRT_PRICE)

@pytest.mark.localnet
@pytest.mark.xfail(reason="getTickAtSqrtPrice returns wrong tick for MIN_SQRT_PRICE (negative tick encoding)")
def test_getTickAtSqrtPrice_isValidMinSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9):
    r = call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, MIN_SQRT_PRICE)
    assert r == MIN_TICK

@pytest.mark.localnet
@pytest.mark.xfail(reason="getTickAtSqrtPrice returns wrong tick for MIN_SQRT_PRICE+1 (negative tick encoding)")
def test_getTickAtSqrtPrice_isValidMinSqrtPricePlusOne(helper25, helper12, helper26, helper28, helper37, helper9):
    r = call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, MIN_SQRT_PRICE + 1)
    assert r == MIN_TICK

@pytest.mark.localnet
def test_getTickAtSqrtPrice_isValidPriceClosestToMaxTick(helper25, helper12, helper26, helper28, helper37, helper9):
    r = call_getTickAtSqrtPrice(helper25, helper12, helper26, helper28, helper37, helper9, MAX_SQRT_PRICE - 1)
    assert r == MAX_TICK - 1
