"""
AAVE V4 Premium library tests.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

UINT256_MAX = 2**256 - 1
RAY = 10**27


@pytest.fixture(scope="module")
def premium(localnet, account):
    return deploy_contract(localnet, account, "PremiumWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def test_deploy(premium):
    assert premium.app_id > 0


def test_calculatePremiumRay_basic(premium):
    """premiumShares * drawnIndex - premiumOffsetRay"""
    # 100 shares, 0 offset, 1 RAY drawn index
    result = _call(premium, "calculatePremiumRay", 100, 0, RAY)
    assert result == 100 * RAY


def test_calculatePremiumRay_with_offset(premium):
    """Result = (shares * index) - offset"""
    shares = 1000
    offset = 500 * RAY  # positive int256 value
    index = RAY
    # (1000 * RAY) - 500*RAY = 500*RAY
    result = _call(premium, "calculatePremiumRay", shares, offset, index)
    assert result == 500 * RAY


def test_calculatePremiumRay_zero_shares(premium):
    """Zero shares should give zero minus offset (if offset is negative)."""
    result = _call(premium, "calculatePremiumRay", 0, 0, RAY)
    assert result == 0


def test_calculatePremiumRay_zero_index(premium):
    """Zero index means zero premium minus offset."""
    result = _call(premium, "calculatePremiumRay", 100, 0, 0)
    assert result == 0


def test_calculatePremiumRay_large_values(premium):
    """Test with large but valid values."""
    shares = 10**18
    index = 2 * RAY  # 2.0 drawn index
    offset = 10**18 * RAY  # offset equals shares * 1 RAY
    # Result = (10^18 * 2*RAY) - (10^18 * RAY) = 10^18 * RAY
    result = _call(premium, "calculatePremiumRay", shares, offset, index)
    assert result == shares * RAY
