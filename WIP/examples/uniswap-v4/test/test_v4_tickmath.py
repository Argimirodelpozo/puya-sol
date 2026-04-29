"""Uniswap V4 TickMath — adapted from TickMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MIN_TICK, MAX_TICK, MIN_SQRT_PRICE, MAX_SQRT_PRICE
from helpers import call_getSqrtPriceAtTick, call_getTickAtSqrtPrice, to_int256

@pytest.mark.localnet
def test_getSqrtPriceAtTick_throwsForTooLow(helper31, helper46, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        call_getSqrtPriceAtTick(helper31, helper46, MIN_TICK - 1,
        orchestrator=orchestrator, algod=algod_client, account=account)

@pytest.mark.localnet
def test_getSqrtPriceAtTick_throwsForTooHigh(helper31, helper46, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        call_getSqrtPriceAtTick(helper31, helper46, MAX_TICK + 1,
        orchestrator=orchestrator, algod=algod_client, account=account)

@pytest.mark.localnet
def test_getSqrtPriceAtTick_isValidMinTick(helper31, helper46, orchestrator, algod_client, account):
    r = call_getSqrtPriceAtTick(helper31, helper46, MIN_TICK,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == MIN_SQRT_PRICE

@pytest.mark.localnet
def test_getSqrtPriceAtTick_isValidMinTickAddOne(helper31, helper46, orchestrator, algod_client, account):
    r = call_getSqrtPriceAtTick(helper31, helper46, MIN_TICK + 1,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == 4295343490

@pytest.mark.localnet
@pytest.mark.xfail(reason="getSqrtPriceAtTick off-by-one: returns MAX_SQRT_PRICE instead of MAX_SQRT_PRICE-1")
def test_getSqrtPriceAtTick_isValidMaxTick(helper31, helper46, orchestrator, algod_client, account):
    r = call_getSqrtPriceAtTick(helper31, helper46, MAX_TICK,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == MAX_SQRT_PRICE - 1  # MAX_SQRT_PRICE is exclusive

@pytest.mark.localnet
def test_getSqrtPriceAtTick_isValidMaxTickSubOne(helper31, helper46, orchestrator, algod_client, account):
    r = call_getSqrtPriceAtTick(helper31, helper46, MAX_TICK - 1,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == 1461373636630004318706518188784493106690254656249

@pytest.mark.localnet
def test_getTickAtSqrtPrice_throwsForTooLow(helper29, helper27, helper28, helper8, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        call_getTickAtSqrtPrice(helper29, helper27, helper28, MIN_SQRT_PRICE - 1, helper8=helper8,
        orchestrator=orchestrator, algod=algod_client, account=account)

@pytest.mark.localnet
def test_getTickAtSqrtPrice_throwsForTooHigh(helper29, helper27, helper28, helper8, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        call_getTickAtSqrtPrice(helper29, helper27, helper28, MAX_SQRT_PRICE, helper8=helper8,
        orchestrator=orchestrator, algod=algod_client, account=account)

@pytest.mark.localnet
@pytest.mark.xfail(reason="getTickAtSqrtPrice overflow: int256 signed multiplication in log2 computation exceeds biguint range")
def test_getTickAtSqrtPrice_isValidMinSqrtPrice(helper29, helper27, helper28, helper8, orchestrator, algod_client, account):
    r = call_getTickAtSqrtPrice(helper29, helper27, helper28, MIN_SQRT_PRICE, helper8=helper8,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == MIN_TICK

@pytest.mark.localnet
@pytest.mark.xfail(reason="getTickAtSqrtPrice overflow: int256 signed multiplication in log2 computation exceeds biguint range")
def test_getTickAtSqrtPrice_isValidMinSqrtPricePlusOne(helper29, helper27, helper28, helper8, orchestrator, algod_client, account):
    r = call_getTickAtSqrtPrice(helper29, helper27, helper28, MIN_SQRT_PRICE + 1, helper8=helper8,
        orchestrator=orchestrator, algod=algod_client, account=account)
    assert r == MIN_TICK

@pytest.mark.localnet
def test_getTickAtSqrtPrice_isValidPriceClosestToMaxTick(helper29, helper27, helper28, helper8, orchestrator, algod_client, account, budget_pad_id):
    r = call_getTickAtSqrtPrice(helper29, helper27, helper28, MAX_SQRT_PRICE - 1, helper8=helper8,
        orchestrator=orchestrator, algod=algod_client, account=account, budget_pad_id=budget_pad_id)
    assert r == MAX_TICK - 1
