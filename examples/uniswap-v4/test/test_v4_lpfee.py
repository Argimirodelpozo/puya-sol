"""Uniswap V4 LPFeeLibrary — adapted from LPFeeLibrary.t.sol"""
import pytest
import algokit_utils as au
from constants import MAX_LP_FEE, DYNAMIC_FEE_FLAG

@pytest.mark.localnet
def test_isDynamicFee_returnsTrue(helper16):
    r = helper16.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.isDynamicFee", args=[DYNAMIC_FEE_FLAG],
    ))
    assert r.abi_return != 0

@pytest.mark.localnet
@pytest.mark.parametrize("fee", [0, 500, 3000, MAX_LP_FEE])
def test_isDynamicFee_returnsFalse(helper16, fee):
    r = helper16.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.isDynamicFee", args=[fee],
    ))
    assert r.abi_return == 0

@pytest.mark.localnet
def test_validate_doesNotRevertWithNoFee(helper32):
    helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.validate", args=[0],
    ))

@pytest.mark.localnet
def test_validate_doesNotRevert(helper32):
    helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.validate", args=[3000],
    ))

@pytest.mark.localnet
def test_validate_doesNotRevertWithMaxFee(helper32):
    helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.validate", args=[MAX_LP_FEE],
    ))

@pytest.mark.localnet
def test_validate_revertsWithLPFeeTooLarge(helper32):
    with pytest.raises(Exception):
        helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.validate", args=[MAX_LP_FEE + 1],
        ))

@pytest.mark.localnet
def test_getInitialLPFee_forStaticFeeIsCorrect(helper35):
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.getInitialLPFee", args=[3000],
    ))
    assert r.abi_return == 3000

@pytest.mark.localnet
def test_getInitialLPFee_revertsWithLPFeeTooLarge(helper35):
    with pytest.raises(Exception):
        helper35.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.getInitialLPFee", args=[MAX_LP_FEE + 1],
        ))

@pytest.mark.localnet
def test_getInitialLPFee_forDynamicFeeIsZero(helper35):
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.getInitialLPFee", args=[DYNAMIC_FEE_FLAG],
    ))
    assert r.abi_return == 0

@pytest.mark.localnet
def test_getInitialLPFee_revertsWithNonExactDynamicFee(helper35):
    with pytest.raises(Exception):
        helper35.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.getInitialLPFee", args=[DYNAMIC_FEE_FLAG + 1],
        ))

@pytest.mark.localnet
@pytest.mark.parametrize("fee,expected", [
    (0, 0),
    (500, 500),
    (MAX_LP_FEE, MAX_LP_FEE),
    (DYNAMIC_FEE_FLAG, 0),
])
def test_fuzz_getInitialLPFee(helper35, fee, expected):
    r = helper35.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.getInitialLPFee", args=[fee],
    ))
    assert r.abi_return == expected

@pytest.mark.localnet
def test_isValid_true(helper32):
    r = helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.isValid", args=[3000],
    ))
    assert r.abi_return != 0

@pytest.mark.localnet
def test_isValid_false(helper32):
    r = helper32.send.call(au.AppClientMethodCallParams(
        method="LPFeeLibrary.isValid", args=[MAX_LP_FEE + 1],
    ))
    assert r.abi_return == 0
