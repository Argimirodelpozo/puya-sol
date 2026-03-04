"""Uniswap V4 SqrtPriceMath — adapted from SqrtPriceMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT160, MAX_UINT128, Q96, Q128, SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_revertsIfPriceIsZero(helper8):
    with pytest.raises(Exception):
        helper8.send.call(au.AppClientMethodCallParams(
            method="SqrtPriceMath.getNextSqrtPriceFromInput",
            args=[0, 1, 10**17, False],
        ))

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_revertsIfLiquidityIsZero(helper8):
    with pytest.raises(Exception):
        helper8.send.call(au.AppClientMethodCallParams(
            method="SqrtPriceMath.getNextSqrtPriceFromInput",
            args=[1, 0, 10**17, True],
        ))

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_returnsInputPriceIfAmountInIsZero_zeroForOne(helper8):
    r = helper8.send.call(au.AppClientMethodCallParams(
        method="SqrtPriceMath.getNextSqrtPriceFromInput",
        args=[SQRT_PRICE_1_1, 10**18, 0, True],
    ))
    assert r.abi_return == SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_returnsInputPriceIfAmountInIsZero_oneForZero(helper8):
    r = helper8.send.call(au.AppClientMethodCallParams(
        method="SqrtPriceMath.getNextSqrtPriceFromInput",
        args=[SQRT_PRICE_1_1, 10**18, 0, False],
    ))
    assert r.abi_return == SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromOutput_revertsIfPriceIsZero(helper8):
    with pytest.raises(Exception):
        helper8.send.call(au.AppClientMethodCallParams(
            method="SqrtPriceMath.getNextSqrtPriceFromOutput",
            args=[0, 1, 10**17, False],
        ))

@pytest.mark.localnet
def test_getNextSqrtPriceFromOutput_revertsIfLiquidityIsZero(helper8):
    with pytest.raises(Exception):
        helper8.send.call(au.AppClientMethodCallParams(
            method="SqrtPriceMath.getNextSqrtPriceFromOutput",
            args=[1, 0, 10**17, True],
        ))

@pytest.mark.localnet
def test_absDiff_a_gt_b(helper32):
    r = helper32.send.call(au.AppClientMethodCallParams(
        method="SqrtPriceMath.absDiff", args=[300, 100],
    ))
    assert r.abi_return == 200

@pytest.mark.localnet
def test_absDiff_b_gt_a(helper32):
    r = helper32.send.call(au.AppClientMethodCallParams(
        method="SqrtPriceMath.absDiff", args=[100, 300],
    ))
    assert r.abi_return == 200

@pytest.mark.localnet
def test_absDiff_equal(helper32):
    r = helper32.send.call(au.AppClientMethodCallParams(
        method="SqrtPriceMath.absDiff", args=[500, 500],
    ))
    assert r.abi_return == 0
