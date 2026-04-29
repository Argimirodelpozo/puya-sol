"""
AAVE V4 SharesMath library tests.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

VIRTUAL_ASSETS = 10**6
VIRTUAL_SHARES = 10**6


@pytest.fixture(scope="module")
def shares(localnet, account):
    return deploy_contract(localnet, account, "SharesMathWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def test_deploy(shares):
    assert shares.app_id > 0


def test_virtual_assets(shares):
    assert _call(shares, "VIRTUAL_ASSETS") == VIRTUAL_ASSETS


def test_virtual_shares(shares):
    assert _call(shares, "VIRTUAL_SHARES") == VIRTUAL_SHARES


# ─── toSharesDown ──────────────────────────────────────────────────────────────

def test_toSharesDown_zero_assets(shares):
    assert _call(shares, "toSharesDown", 0, 1000, 1000) == 0


def test_toSharesDown_basic(shares):
    # assets=100, totalAssets=1000, totalShares=1000
    # result = 100 * (1000 + 1e6) / (1000 + 1e6) = 100
    assert _call(shares, "toSharesDown", 100, 1000, 1000) == 100


def test_toSharesDown_zero_totals(shares):
    # assets=100, totalAssets=0, totalShares=0
    # result = 100 * (0 + 1e6) / (0 + 1e6) = 100
    assert _call(shares, "toSharesDown", 100, 0, 0) == 100


def test_toSharesDown_asymmetric(shares):
    # assets=500, totalAssets=1000, totalShares=2000
    # result = 500 * (2000 + 1e6) / (1000 + 1e6) = floor(500 * 1002000 / 1001000)
    expected = 500 * (2000 + VIRTUAL_SHARES) // (1000 + VIRTUAL_ASSETS)
    assert _call(shares, "toSharesDown", 500, 1000, 2000) == expected


# ─── toAssetsDown ──────────────────────────────────────────────────────────────

def test_toAssetsDown_zero_shares(shares):
    assert _call(shares, "toAssetsDown", 0, 1000, 1000) == 0


def test_toAssetsDown_basic(shares):
    assert _call(shares, "toAssetsDown", 100, 1000, 1000) == 100


def test_toAssetsDown_zero_totals(shares):
    assert _call(shares, "toAssetsDown", 100, 0, 0) == 100


# ─── toSharesUp ────────────────────────────────────────────────────────────────

def test_toSharesUp_zero_assets(shares):
    assert _call(shares, "toSharesUp", 0, 1000, 1000) == 0


def test_toSharesUp_basic(shares):
    assert _call(shares, "toSharesUp", 100, 1000, 1000) == 100


def test_toSharesUp_rounds_up(shares):
    # If not exact division, should round up
    # assets=1, totalAssets=3, totalShares=1
    # result = ceil(1 * (1 + 1e6) / (3 + 1e6))
    num = 1 * (1 + VIRTUAL_SHARES)
    den = 3 + VIRTUAL_ASSETS
    expected = (num + den - 1) // den
    assert _call(shares, "toSharesUp", 1, 3, 1) == expected


# ─── toAssetsUp ────────────────────────────────────────────────────────────────

def test_toAssetsUp_zero_shares(shares):
    assert _call(shares, "toAssetsUp", 0, 1000, 1000) == 0


def test_toAssetsUp_basic(shares):
    assert _call(shares, "toAssetsUp", 100, 1000, 1000) == 100


def test_toAssetsUp_rounds_up(shares):
    # shares=1, totalAssets=1, totalShares=3
    num = 1 * (1 + VIRTUAL_ASSETS)
    den = 3 + VIRTUAL_SHARES
    expected = (num + den - 1) // den
    assert _call(shares, "toAssetsUp", 1, 1, 3) == expected


# ─── Roundtrip consistency ────────────────────────────────────────────────────

def test_roundtrip_down(shares):
    """Converting assets→shares→assets (both down) should not increase."""
    assets = 1000
    total_assets = 5000
    total_shares = 5000
    s = _call(shares, "toSharesDown", assets, total_assets, total_shares)
    a = _call(shares, "toAssetsDown", s, total_assets, total_shares)
    assert a <= assets


def test_roundtrip_up(shares):
    """Converting assets→shares→assets (both up) should not decrease."""
    assets = 1000
    total_assets = 5000
    total_shares = 5000
    s = _call(shares, "toSharesUp", assets, total_assets, total_shares)
    a = _call(shares, "toAssetsUp", s, total_assets, total_shares)
    assert a >= assets
