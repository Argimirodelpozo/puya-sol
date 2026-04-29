"""Uniswap V4 SqrtPriceMath — adapted from SqrtPriceMath.t.sol"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import MAX_UINT160, MAX_UINT128, Q96, Q128, SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_revertsIfPriceIsZero(helper9, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper9, "SqrtPriceMath.getNextSqrtPriceFromInput", [0, 1, 10**17, False], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_revertsIfLiquidityIsZero(helper9, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper9, "SqrtPriceMath.getNextSqrtPriceFromInput", [1, 0, 10**17, True], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_returnsInputPriceIfAmountInIsZero_zeroForOne(helper9, orchestrator, algod_client, account):
    r = grouped_call(helper9, "SqrtPriceMath.getNextSqrtPriceFromInput", [SQRT_PRICE_1_1, 10**18, 0, True], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromInput_returnsInputPriceIfAmountInIsZero_oneForZero(helper9, orchestrator, algod_client, account):
    r = grouped_call(helper9, "SqrtPriceMath.getNextSqrtPriceFromInput", [SQRT_PRICE_1_1, 10**18, 0, False], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1

@pytest.mark.localnet
def test_getNextSqrtPriceFromOutput_revertsIfPriceIsZero(helper10, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper10, "SqrtPriceMath.getNextSqrtPriceFromOutput", [0, 1, 10**17, False], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_getNextSqrtPriceFromOutput_revertsIfLiquidityIsZero(helper10, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        grouped_call(helper10, "SqrtPriceMath.getNextSqrtPriceFromOutput", [1, 0, 10**17, True], orchestrator, algod_client, account)

@pytest.mark.localnet
def test_absDiff_a_gt_b(helper47, orchestrator, algod_client, account):
    r = grouped_call(helper47, "SqrtPriceMath.absDiff", [300, 100], orchestrator, algod_client, account)
    assert r == 200

@pytest.mark.localnet
def test_absDiff_b_gt_a(helper47, orchestrator, algod_client, account):
    r = grouped_call(helper47, "SqrtPriceMath.absDiff", [100, 300], orchestrator, algod_client, account)
    assert r == 200

@pytest.mark.localnet
def test_absDiff_equal(helper47, orchestrator, algod_client, account):
    r = grouped_call(helper47, "SqrtPriceMath.absDiff", [500, 500], orchestrator, algod_client, account)
    assert r == 0
