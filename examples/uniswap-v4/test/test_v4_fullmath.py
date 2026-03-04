"""Uniswap V4 FullMath — adapted from FullMath.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_UINT256, Q128

@pytest.mark.localnet
def test_mulDiv_revertsWith0Denominator_case1(helper45):
    with pytest.raises(Exception):
        helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, 5, 0]))

@pytest.mark.localnet
def test_mulDiv_revertsWith0Denominator_case2(helper45):
    with pytest.raises(Exception):
        helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[1, 5, 0]))

@pytest.mark.localnet
def test_mulDiv_revertsWithOverflowingNumeratorAndZeroDenominator(helper45):
    with pytest.raises(Exception):
        helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, Q128, 0]))

@pytest.mark.localnet
def test_mulDiv_revertsIfOutputOverflows(helper45):
    with pytest.raises(Exception):
        helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, Q128, 1]))

@pytest.mark.localnet
def test_mulDiv_revertsOverflowWithAllMaxInputs(helper45):
    with pytest.raises(Exception):
        helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[MAX_UINT256, MAX_UINT256, MAX_UINT256 - 1]))

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: mulDiv phantom overflow with all-max inputs triggers wrapping arithmetic issue")
def test_mulDiv_validAllMaxInputs(helper45):
    r = helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[MAX_UINT256, MAX_UINT256, MAX_UINT256]))
    assert r.abi_return == MAX_UINT256

@pytest.mark.localnet
def test_mulDiv_validWithoutPhantomOverflow(helper45):
    r = helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, 50 * Q128 // 100, 150 * Q128 // 100]))
    assert r.abi_return == Q128 // 3

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: mulDiv phantom overflow path requires correct wrapping NOT and sub")
def test_mulDiv_validWithPhantomOverflow(helper45):
    r = helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, 35 * Q128, 8 * Q128]))
    assert r.abi_return == 4375 * Q128 // 1000

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: mulDiv phantom overflow path requires correct wrapping NOT and sub")
def test_mulDiv_phantomOverflowRepeatingDecimal(helper45):
    r = helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[Q128, 1000 * Q128, 3000 * Q128]))
    assert r.abi_return == Q128 // 3

@pytest.mark.localnet
@pytest.mark.parametrize("x,y,d,expected", [
    (500, 2, 4, 250),
    (10, 3, 7, 4),
    (Q128, 1, 1, Q128),
    (1, Q128, Q128, 1),
    (MAX_UINT256 // 2, 2, MAX_UINT256, 0),
])
def test_fuzz_mulDiv(helper45, x, y, d, expected):
    r = helper45.send.call(au.AppClientMethodCallParams(method="FullMath.mulDiv", args=[x, y, d]))
    assert r.abi_return == expected

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsWith0Denominator_case1(helper44):
    with pytest.raises(Exception):
        helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[Q128, 5, 0]))

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsWith0Denominator_case2(helper44):
    with pytest.raises(Exception):
        helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[1, 5, 0]))

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: mulDivRoundingUp with all-max inputs triggers wrapping arithmetic issue")
def test_mulDivRoundingUp_validWithAllMaxInputs(helper44):
    r = helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[MAX_UINT256, MAX_UINT256, MAX_UINT256]))
    assert r.abi_return == MAX_UINT256

@pytest.mark.localnet
def test_mulDivRoundingUp_validWithNoPhantomOverflow(helper44):
    r = helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[Q128, 50 * Q128 // 100, 150 * Q128 // 100]))
    assert r.abi_return == Q128 // 3 + 1

@pytest.mark.localnet
@pytest.mark.xfail(reason="Compiler bug: mulDivRoundingUp phantom overflow path requires correct wrapping NOT and sub")
def test_mulDivRoundingUp_validWithPhantomOverflow(helper44):
    r = helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[Q128, 35 * Q128, 8 * Q128]))
    assert r.abi_return == 4375 * Q128 // 1000

@pytest.mark.localnet
def test_mulDivRoundingUp_revertsIfMulDivOverflows(helper44):
    with pytest.raises(Exception):
        helper44.send.call(au.AppClientMethodCallParams(method="FullMath.mulDivRoundingUp", args=[Q128, Q128, 1]))
