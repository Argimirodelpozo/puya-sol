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
    # Unique note disambiguates identical (method, args) pairs called
    # multiple times against the same app — algod dedupes by full
    # txn-hash and would 400 with "transaction already in ledger"
    # otherwise. Cheaper than mutating suggestedParams or jittering fees.
    import os
    result = client.send.call(au.AppClientMethodCallParams(
        method=method, args=list(args), note=os.urandom(8),
    ))
    return result.abi_return


def _send(client, method, *args):
    import os
    client.send.call(au.AppClientMethodCallParams(
        method=method, args=list(args), note=os.urandom(8),
    ))


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


# ─── Ported from upstream PositionStatusMap.t.sol ─────────────────────────────
# Concrete-value tests covering the corner reserveIds at slot boundaries
# (0/127 in slot0, 128/255 in slot1) and count/fls behaviour.
# Skipped: vm.setArbitraryStorage / vm.record / vm.randomBool / fuzz cases.


def _fresh(localnet, account):
    """Each test gets its own wrapper instance — the bitmap is mutated
    across helper methods, so isolating per-test prevents cross-talk."""
    return deploy_contract(localnet, account, "PositionStatusMapWrapper")


def test_setBorrowing_slot0(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setBorrowing", 0, True)
    assert _call(p, "isBorrowing", 0) == True
    _send(p, "setBorrowing", 0, False)
    assert _call(p, "isBorrowing", 0) == False
    _send(p, "setBorrowing", 127, True)
    assert _call(p, "isBorrowing", 127) == True
    _send(p, "setBorrowing", 127, False)
    assert _call(p, "isBorrowing", 127) == False


def test_setBorrowing_slot1(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setBorrowing", 128, True)
    assert _call(p, "isBorrowing", 128) == True
    _send(p, "setBorrowing", 128, False)
    assert _call(p, "isBorrowing", 128) == False
    _send(p, "setBorrowing", 255, True)
    assert _call(p, "isBorrowing", 255) == True
    _send(p, "setBorrowing", 255, False)
    assert _call(p, "isBorrowing", 255) == False


def test_setUseAsCollateral_slot0(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 0, True)
    assert _call(p, "isUsingAsCollateral", 0) == True
    _send(p, "setUsingAsCollateral", 0, False)
    assert _call(p, "isUsingAsCollateral", 0) == False
    _send(p, "setUsingAsCollateral", 127, True)
    assert _call(p, "isUsingAsCollateral", 127) == True
    _send(p, "setUsingAsCollateral", 127, False)
    assert _call(p, "isUsingAsCollateral", 127) == False


def test_setUseAsCollateral_slot1(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 128, True)
    assert _call(p, "isUsingAsCollateral", 128) == True
    _send(p, "setUsingAsCollateral", 128, False)
    assert _call(p, "isUsingAsCollateral", 128) == False
    _send(p, "setUsingAsCollateral", 255, True)
    assert _call(p, "isUsingAsCollateral", 255) == True
    _send(p, "setUsingAsCollateral", 255, False)
    assert _call(p, "isUsingAsCollateral", 255) == False


def test_isUsingAsCollateralOrBorrowing_slot0(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 0, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == True
    _send(p, "setUsingAsCollateral", 0, False)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == False
    _send(p, "setBorrowing", 0, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == True
    _send(p, "setBorrowing", 0, False)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == False
    # Both set then both cleared.
    _send(p, "setUsingAsCollateral", 0, True)
    _send(p, "setBorrowing", 0, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == True
    _send(p, "setUsingAsCollateral", 0, False)
    _send(p, "setBorrowing", 0, False)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 0) == False
    # Boundary reserveId 127 still in slot 0.
    _send(p, "setUsingAsCollateral", 127, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 127) == True
    _send(p, "setUsingAsCollateral", 127, False)


def test_isUsingAsCollateralOrBorrowing_slot1(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 128, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 128) == True
    _send(p, "setUsingAsCollateral", 128, False)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 128) == False
    _send(p, "setUsingAsCollateral", 255, True)
    assert _call(p, "isUsingAsCollateralOrBorrowing", 255) == True
    _send(p, "setUsingAsCollateral", 255, False)


# puya-sol codegen bug: lowering of `self.map[--bucket]` emits TWO `b-`
# ops in collateralCount/borrowCount loop bodies. The first computes
# bucket-1 (used as box key), the second pops bucket-1 from the stack
# and subtracts again, producing bucket-2 — when bucket=1 the result
# is -1, tripping "byte math would have negative result". Affects all
# reserveCounts ≥ 128. Tests below xfail until puya-sol fixes its
# pre-decrement-in-subscript codegen.
_PSM_BUCKET_LOOP_XFAIL = pytest.mark.xfail(
    reason="puya-sol --bucket-in-subscript double-b- codegen bug",
    strict=True,
)


@_PSM_BUCKET_LOOP_XFAIL
def test_collateralCount(localnet, account):
    """Mirrors upstream test_collateralCount() — exercises:
    boundary reserveIds, ignoring bits past reserveCount, and that
    counts ignore borrow bits."""
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 127, True)
    assert _call(p, "collateralCount", 128) == 1
    _send(p, "setUsingAsCollateral", 128, True)
    assert _call(p, "collateralCount", 128) == 1   # 128 not yet in range
    assert _call(p, "collateralCount", 129) == 2
    # ignore invalid bits.
    assert _call(p, "collateralCount", 100) == 0
    _send(p, "setUsingAsCollateral", 2, True)
    assert _call(p, "collateralCount", 128) == 2
    _send(p, "setUsingAsCollateral", 32, True)
    assert _call(p, "collateralCount", 128) == 3
    _send(p, "setUsingAsCollateral", 342, True)
    assert _call(p, "collateralCount", 343) == 5
    _send(p, "setUsingAsCollateral", 32, False)
    assert _call(p, "collateralCount", 343) == 4
    # disregards borrowed reserves.
    _send(p, "setBorrowing", 32, True)
    assert _call(p, "collateralCount", 343) == 4


@_PSM_BUCKET_LOOP_XFAIL
def test_collateralCount_ignoresInvalidBits(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setUsingAsCollateral", 127, True)
    assert _call(p, "collateralCount", 100) == 0
    assert _call(p, "collateralCount", 200) == 1
    _send(p, "setUsingAsCollateral", 255, True)
    assert _call(p, "collateralCount", 200) == 1
    _send(p, "setUsingAsCollateral", 133, True)
    assert _call(p, "collateralCount", 200) == 2
    _send(p, "setUsingAsCollateral", 383, True)
    assert _call(p, "collateralCount", 300) == 3
    _send(p, "setUsingAsCollateral", 283, True)
    assert _call(p, "collateralCount", 300) == 4
    _send(p, "setUsingAsCollateral", 511, True)
    assert _call(p, "collateralCount", 500) == 5
    assert _call(p, "collateralCount", 600) == 6


@_PSM_BUCKET_LOOP_XFAIL
def test_borrowCount(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setBorrowing", 127, True)
    assert _call(p, "borrowCount", 128) == 1
    _send(p, "setBorrowing", 128, True)
    assert _call(p, "borrowCount", 128) == 1
    assert _call(p, "borrowCount", 129) == 2
    assert _call(p, "borrowCount", 100) == 0
    _send(p, "setBorrowing", 2, True)
    assert _call(p, "borrowCount", 128) == 2
    _send(p, "setBorrowing", 32, True)
    assert _call(p, "borrowCount", 128) == 3
    _send(p, "setBorrowing", 342, True)
    assert _call(p, "borrowCount", 343) == 5
    _send(p, "setBorrowing", 32, False)
    assert _call(p, "borrowCount", 343) == 4
    # disregards collateral reserves.
    _send(p, "setUsingAsCollateral", 32, True)
    assert _call(p, "borrowCount", 343) == 4


@_PSM_BUCKET_LOOP_XFAIL
def test_borrowCount_ignoresInvalidBits(localnet, account):
    p = _fresh(localnet, account)
    _send(p, "setBorrowing", 127, True)
    assert _call(p, "borrowCount", 100) == 0
    assert _call(p, "borrowCount", 200) == 1
    _send(p, "setBorrowing", 255, True)
    assert _call(p, "borrowCount", 200) == 1
    _send(p, "setBorrowing", 133, True)
    assert _call(p, "borrowCount", 200) == 2
    _send(p, "setBorrowing", 383, True)
    assert _call(p, "borrowCount", 300) == 3
    _send(p, "setBorrowing", 283, True)
    assert _call(p, "borrowCount", 300) == 4
    _send(p, "setBorrowing", 511, True)
    assert _call(p, "borrowCount", 500) == 5
    assert _call(p, "borrowCount", 600) == 6


def test_fls_returns_floor_log2(psm):
    """LibBit.fls(x) returns the position of the highest set bit
    (floor of log2). Direct port of upstream test_fls()."""
    # 0xff << 3 = 0x7f8 = 0b11111111000 → highest bit at index 10.
    assert _call(psm, "fls", 0xff << 3) == 10
    # For each i in [1, 254]: fls(2^i - 1) == i - 1, fls(2^i) == i,
    # fls(2^i + 1) == i. Loop in chunks to keep the test fast.
    for i in range(1, 255):
        assert _call(psm, "fls", (1 << i) - 1) == i - 1
        assert _call(psm, "fls", (1 << i)) == i
        assert _call(psm, "fls", (1 << i) + 1) == i
    # Empty word: sentinel 256 (no bits set).
    assert _call(psm, "fls", 0) == 256


def test_constants_construction(psm):
    """Reproduces upstream test_constants — the masks should be
    constructible via the bit-by-bit recipe (even bits = borrow,
    odd bits = collateral) and partition uint256."""
    expected_borrow = 0
    expected_collat = 0
    for i in range(0, 256, 2):
        expected_borrow |= (1 << i)
        expected_collat |= (1 << (i + 1))
    assert _call(psm, "BORROWING_MASK") == expected_borrow
    assert _call(psm, "COLLATERAL_MASK") == expected_collat
    # The two masks partition all 256 bits.
    UINT256_MAX = (1 << 256) - 1
    assert (_call(psm, "BORROWING_MASK") | _call(psm, "COLLATERAL_MASK")) == UINT256_MAX
    assert (_call(psm, "BORROWING_MASK") & _call(psm, "COLLATERAL_MASK")) == 0
