"""
AAVE V4 PositionStatusMap library tests.
Translated from PositionStatusMap.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

BORROWING_MASK = 0x5555555555555555555555555555555555555555555555555555555555555555
COLLATERAL_MASK = 0xAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
NOT_FOUND = 2**256 - 1


@pytest.fixture(scope="module")
def psm(localnet, account):
    return deploy_contract(localnet, account, "PositionStatusMapWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


def _send(client, method, *args):
    client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))


# ─── Constants ─────────────────────────────────────────────────────────────────

def test_deploy(psm):
    assert psm.app_id > 0


def test_borrowing_mask(psm):
    assert _call(psm, "BORROWING_MASK") == BORROWING_MASK


def test_collateral_mask(psm):
    assert _call(psm, "COLLATERAL_MASK") == COLLATERAL_MASK


# ─── Pure functions ────────────────────────────────────────────────────────────

def test_bucketId(psm):
    # Each bucket holds 128 reserves
    assert _call(psm, "bucketId", 0) == 0
    assert _call(psm, "bucketId", 127) == 0
    assert _call(psm, "bucketId", 128) == 1
    assert _call(psm, "bucketId", 255) == 1


def test_isolateBorrowing(psm):
    # Borrowing bits are in even positions (mask 0x5555...)
    assert _call(psm, "isolateBorrowing", BORROWING_MASK) == BORROWING_MASK
    assert _call(psm, "isolateBorrowing", COLLATERAL_MASK) == 0
    assert _call(psm, "isolateBorrowing", 0) == 0


def test_isolateCollateral(psm):
    # Collateral bits are in odd positions (mask 0xAAAA...)
    assert _call(psm, "isolateCollateral", COLLATERAL_MASK) == COLLATERAL_MASK
    assert _call(psm, "isolateCollateral", BORROWING_MASK) == 0
    assert _call(psm, "isolateCollateral", 0) == 0


# ─── State operations ─────────────────────────────────────────────────────────

def test_setBorrowing_and_isBorrowing(psm):
    _send(psm, "setBorrowing", 0, True)
    assert _call(psm, "isBorrowing", 0) == True
    _send(psm, "setBorrowing", 0, False)
    assert _call(psm, "isBorrowing", 0) == False


def test_setCollateral_and_isCollateral(psm):
    _send(psm, "setUsingAsCollateral", 0, True)
    assert _call(psm, "isUsingAsCollateral", 0) == True
    _send(psm, "setUsingAsCollateral", 0, False)
    assert _call(psm, "isUsingAsCollateral", 0) == False


def test_isUsingAsCollateralOrBorrowing(psm):
    # Neither set
    assert _call(psm, "isUsingAsCollateralOrBorrowing", 5) == False
    # Set borrowing
    _send(psm, "setBorrowing", 5, True)
    assert _call(psm, "isUsingAsCollateralOrBorrowing", 5) == True
    # Clear borrowing, set collateral
    _send(psm, "setBorrowing", 5, False)
    _send(psm, "setUsingAsCollateral", 5, True)
    assert _call(psm, "isUsingAsCollateralOrBorrowing", 5) == True
    # Clear both
    _send(psm, "setUsingAsCollateral", 5, False)
    assert _call(psm, "isUsingAsCollateralOrBorrowing", 5) == False


def test_multiple_reserves(psm):
    """Setting different reserves should be independent."""
    _send(psm, "setBorrowing", 10, True)
    _send(psm, "setUsingAsCollateral", 20, True)
    assert _call(psm, "isBorrowing", 10) == True
    assert _call(psm, "isUsingAsCollateral", 20) == True
    assert _call(psm, "isBorrowing", 20) == False
    assert _call(psm, "isUsingAsCollateral", 10) == False
    # Clean up
    _send(psm, "setBorrowing", 10, False)
    _send(psm, "setUsingAsCollateral", 20, False)
