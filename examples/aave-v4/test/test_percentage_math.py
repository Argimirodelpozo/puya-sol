"""
AAVE V4 PercentageMath library tests.
Translated from PercentageMath.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

UINT256_MAX = 2**256 - 1
PERCENTAGE_FACTOR = 10**4


@pytest.fixture(scope="module")
def pct(localnet, account):
    return deploy_contract(localnet, account, "PercentageMathWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


# ─── Constants ────────────────────────────────────────────────────────────────

def test_deploy(pct):
    assert pct.app_id > 0


def test_constants(pct):
    assert _call(pct, "PERCENTAGE_FACTOR") == PERCENTAGE_FACTOR


# ─── Concrete percentMul tests ──────────────────────────────────────────────

def test_percentMulDown_basic(pct):
    assert _call(pct, "percentMulDown", 10**18, 5000) == int(0.5e18)
    assert _call(pct, "percentMulDown", 14251500000000000000, 7442) == 10605966300000000000
    assert _call(pct, "percentMulDown", 9087312 * 10**27, 1333) == 1211338689600000000000000000000000


def test_percentMulUp_basic(pct):
    assert _call(pct, "percentMulUp", 10**18, 5000) == int(0.5e18)
    assert _call(pct, "percentMulUp", 14251500000000000000, 7442) == 10605966300000000000
    assert _call(pct, "percentMulUp", 9087312 * 10**27, 1333) == 1211338689600000000000000000000000


# ─── Concrete percentDiv tests ──────────────────────────────────────────────

def test_percentDivDown_basic(pct):
    assert _call(pct, "percentDivDown", 10**18, 5000) == 2 * 10**18
    assert _call(pct, "percentDivDown", 14251500000000000000, 7442) == 19150094060736361193
    assert _call(pct, "percentDivDown", 9087312 * 10**27, 1333) == 68171882970742685671417854463615903


def test_percentDivUp_basic(pct):
    assert _call(pct, "percentDivUp", 10**18, 5000) == 2 * 10**18
    assert _call(pct, "percentDivUp", 14251500000000000000, 7442) == 19150094060736361194
    assert _call(pct, "percentDivUp", 9087312 * 10**27, 1333) == 68171882970742685671417854463615904


# ─── fromBpsDown ─────────────────────────────────────────────────────────────

def test_fromBpsDown(pct):
    assert _call(pct, "fromBpsDown", 10**18) == 10**14
    assert _call(pct, "fromBpsDown", 10**3) == 0


# ─── Division by zero reverts ───────────────────────────────────────────────

def test_percentDivDown_div_by_zero(pct):
    with pytest.raises(Exception):
        _call(pct, "percentDivDown", 10**18, 0)


def test_percentDivUp_div_by_zero(pct):
    with pytest.raises(Exception):
        _call(pct, "percentDivUp", 10**18, 0)


# ─── Overflow reverts ───────────────────────────────────────────────────────

def test_percentMulDown_overflow(pct):
    with pytest.raises(Exception):
        _call(pct, "percentMulDown", UINT256_MAX, UINT256_MAX)


def test_percentMulUp_overflow(pct):
    with pytest.raises(Exception):
        _call(pct, "percentMulUp", UINT256_MAX, UINT256_MAX)


def test_percentDivDown_overflow(pct):
    # value > UINT256_MAX / PERCENTAGE_FACTOR
    big = UINT256_MAX // PERCENTAGE_FACTOR + 1
    with pytest.raises(Exception):
        _call(pct, "percentDivDown", big, 1)


def test_percentDivUp_overflow(pct):
    big = UINT256_MAX // PERCENTAGE_FACTOR + 1
    with pytest.raises(Exception):
        _call(pct, "percentDivUp", big, 1)


# ─── Property tests from Foundry ────────────────────────────────────────────

def test_percentMulDown_zero_value(pct):
    assert _call(pct, "percentMulDown", 0, 5000) == 0


def test_percentMulDown_zero_pct(pct):
    assert _call(pct, "percentMulDown", 10**18, 0) == 0


def test_percentMulUp_identity(pct):
    """percentMulUp by 100% should return the value."""
    assert _call(pct, "percentMulUp", 10**18, PERCENTAGE_FACTOR) == 10**18


def test_percentDivUp_identity(pct):
    """percentDivUp by 100% should return the value."""
    assert _call(pct, "percentDivUp", 10**18, PERCENTAGE_FACTOR) == 10**18


def test_percentDivUp_le_value_ge_100pct(pct):
    """percentDivUp by >= 100% should never exceed the original value."""
    value = 10**18
    for pct_val in [PERCENTAGE_FACTOR, 2 * PERCENTAGE_FACTOR, 10 * PERCENTAGE_FACTOR]:
        result = _call(pct, "percentDivUp", value, pct_val)
        assert result <= value


def test_percentDivUp_ge_value_le_100pct(pct):
    """percentDivUp by <= 100% should always be >= original value."""
    value = 10**18
    for pct_val in [1, 100, 5000, PERCENTAGE_FACTOR]:
        result = _call(pct, "percentDivUp", value, pct_val)
        assert result >= value


def test_percentMulUp_ge_value_ge_100pct(pct):
    """percentMulUp by >= 100% should always be >= original value."""
    value = 10**18
    for pct_val in [PERCENTAGE_FACTOR, 2 * PERCENTAGE_FACTOR, 5 * PERCENTAGE_FACTOR]:
        result = _call(pct, "percentMulUp", value, pct_val)
        assert result >= value


def test_percentMulUp_le_value_le_100pct(pct):
    """percentMulUp by <= 100% should never exceed original value."""
    value = 10**18
    for pct_val in [1, 100, 5000, PERCENTAGE_FACTOR]:
        result = _call(pct, "percentMulUp", value, pct_val)
        assert result <= value
