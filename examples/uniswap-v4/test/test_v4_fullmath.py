"""Uniswap V4 FullMath — adapted from FullMath.t.sol"""
import pytest
from helpers import grouped_call, call_with_budget
import algokit_utils as au
from constants import MAX_UINT256, Q128


def _call_mulDiv(helper36, a, b, denominator, orchestrator, algod_client, account, budget_pad_id=None):
    """Call FullMath.mulDiv (single method on Helper36)."""
    if budget_pad_id:
        return call_with_budget(helper36, "FullMath.mulDiv", [a, b, denominator], budget_pad_id, algod_client, account, orchestrator=orchestrator)
    return grouped_call(helper36, "FullMath.mulDiv", [a, b, denominator], orchestrator, algod_client, account)


def _call_mulDivRoundingUp(helper35, a, b, denominator, orchestrator, algod_client, account, budget_pad_id=None):
    """Call FullMath.mulDivRoundingUp (single method on Helper35)."""
    if budget_pad_id:
        return call_with_budget(helper35, "FullMath.mulDivRoundingUp", [a, b, denominator], budget_pad_id, algod_client, account, orchestrator=orchestrator)
    return grouped_call(helper35, "FullMath.mulDivRoundingUp", [a, b, denominator], orchestrator, algod_client, account)


@pytest.mark.localnet
def test_mulDiv_revertsWith0Denominator_case1(helper36, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDiv(helper36, Q128, 5, 0, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDiv_revertsWith0Denominator_case2(helper36, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDiv(helper36, 1, 5, 0, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDiv_revertsWithOverflowingNumeratorAndZeroDenominator(helper36, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDiv(helper36, Q128, Q128, 0, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDiv_revertsIfOutputOverflows(helper36, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDiv(helper36, Q128, Q128, 1, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDiv_revertsOverflowWithAllMaxInputs(helper36, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDiv(helper36, MAX_UINT256, MAX_UINT256, MAX_UINT256 - 1, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDiv_validAllMaxInputs(helper36, orchestrator, algod_client, account):
    r = _call_mulDiv(helper36, MAX_UINT256, MAX_UINT256, MAX_UINT256, orchestrator, algod_client, account)
    assert r == MAX_UINT256

@pytest.mark.localnet
def test_mulDiv_validWithoutPhantomOverflow(helper36, orchestrator, algod_client, account):
    r = _call_mulDiv(helper36, Q128, 50 * Q128 // 100, 150 * Q128 // 100, orchestrator, algod_client, account)
    assert r == Q128 // 3

@pytest.mark.localnet
def test_mulDiv_validWithPhantomOverflow(helper36, orchestrator, algod_client, account):
    r = _call_mulDiv(helper36, Q128, 35 * Q128, 8 * Q128, orchestrator, algod_client, account)
    assert r == 4375 * Q128 // 1000

@pytest.mark.localnet
def test_mulDiv_phantomOverflowRepeatingDecimal(helper36, orchestrator, algod_client, account):
    r = _call_mulDiv(helper36, Q128, 1000 * Q128, 3000 * Q128, orchestrator, algod_client, account)
    assert r == Q128 // 3

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,d,expected", [
    (500, 2, 4, 250),
    (10, 3, 7, 4),
    (Q128, 1, 1, Q128),
    (1, Q128, Q128, 1),
    (MAX_UINT256 // 2, 2, MAX_UINT256, 0),
])
def test_fuzz_mulDiv(helper36, x, y, d, expected, orchestrator, algod_client, account):
    r = _call_mulDiv(helper36, x, y, d, orchestrator, algod_client, account)
    assert r == expected

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsWith0Denominator_case1(helper35, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDivRoundingUp(helper35, Q128, 5, 0, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsWith0Denominator_case2(helper35, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDivRoundingUp(helper35, 1, 5, 0, orchestrator, algod_client, account)

@pytest.mark.localnet
def test_mulDivRoundingUp_validWithAllMaxInputs(helper35, orchestrator, algod_client, account, budget_pad_id):
    r = _call_mulDivRoundingUp(helper35, MAX_UINT256, MAX_UINT256, MAX_UINT256, orchestrator, algod_client, account, budget_pad_id=budget_pad_id)
    assert r == MAX_UINT256

@pytest.mark.localnet
def test_mulDivRoundingUp_validWithNoPhantomOverflow(helper35, orchestrator, algod_client, account):
    r = _call_mulDivRoundingUp(helper35, Q128, 50 * Q128 // 100, 150 * Q128 // 100, orchestrator, algod_client, account)
    assert r == Q128 // 3 + 1

@pytest.mark.localnet
def test_mulDivRoundingUp_validWithPhantomOverflow(helper35, orchestrator, algod_client, account, budget_pad_id):
    r = _call_mulDivRoundingUp(helper35, Q128, 35 * Q128, 8 * Q128, orchestrator, algod_client, account, budget_pad_id=budget_pad_id)
    assert r == 4375 * Q128 // 1000

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsIfMulDivOverflows(helper35, orchestrator, algod_client, account):
    with pytest.raises(Exception):
        _call_mulDivRoundingUp(helper35, Q128, Q128, 1, orchestrator, algod_client, account)
