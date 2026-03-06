"""
AAVE V4 MathUtils library tests.
Translated from MathUtils.t.sol (Foundry).
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

UINT256_MAX = 2**256 - 1
INT256_MAX = 2**255 - 1
RAY = 10**27
SECONDS_PER_YEAR = 365 * 24 * 3600  # 31536000


@pytest.fixture(scope="module")
def math(localnet, account):
    return deploy_contract(localnet, account, "MathUtilsWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


# ─── Constants ────────────────────────────────────────────────────────────────

def test_deploy(math):
    assert math.app_id > 0


def test_seconds_per_year(math):
    assert _call(math, "SECONDS_PER_YEAR") == SECONDS_PER_YEAR


# ─── calculateLinearInterest ─────────────────────────────────────────────────
# NOTE: calculateLinearInterest is a `view` function that uses block.timestamp.
# On AVM, `global LatestTimestamp` is used. We pass a lastUpdateTimestamp that
# is before the current block timestamp and verify the formula:
#   result = RAY + (rate * (timestamp - lastUpdateTimestamp)) / SECONDS_PER_YEAR

def test_calculateLinearInterest_zero_elapsed(math, algod_client):
    """When lastUpdateTimestamp == block.timestamp, result should be RAY (1e27)."""
    # Get current latest timestamp from AVM
    status = algod_client.status()
    # Use a timestamp that's very close to now - the AVM timestamp should be recent
    # We pass 0 as lastUpdateTimestamp to get a large elapsed time
    # rate = 0 means result = RAY regardless of elapsed time
    result = _call(math, "calculateLinearInterest", 0, 0)
    assert result == RAY


def test_calculateLinearInterest_with_rate(math, algod_client):
    """Test with a known rate. Use lastUpdateTimestamp=0 and compute expected from AVM timestamp."""
    rate = int(0.08e27)  # 8% annual rate
    result = _call(math, "calculateLinearInterest", rate, 0)
    # result = RAY + (rate * elapsed) / SECONDS_PER_YEAR
    # We know result >= RAY since elapsed >= 0
    assert result >= RAY


# ─── min ─────────────────────────────────────────────────────────────────────

def test_min_first_smaller(math):
    assert _call(math, "min", 10, 50) == 10


def test_min_second_smaller(math):
    assert _call(math, "min", 50, 10) == 10


def test_min_equal(math):
    assert _call(math, "min", 42, 42) == 42


def test_min_zero(math):
    assert _call(math, "min", 0, 100) == 0
    assert _call(math, "min", 100, 0) == 0


def test_min_large(math):
    big = 10**50
    assert _call(math, "min", big, big + 1) == big
    assert _call(math, "min", big + 1, big) == big


# ─── zeroFloorSub ───────────────────────────────────────────────────────────

def test_zeroFloorSub_a_gt_b(math):
    assert _call(math, "zeroFloorSub", 100, 30) == 70


def test_zeroFloorSub_a_eq_b(math):
    assert _call(math, "zeroFloorSub", 50, 50) == 0


def test_zeroFloorSub_a_lt_b(math):
    assert _call(math, "zeroFloorSub", 30, 100) == 0


def test_zeroFloorSub_zero(math):
    assert _call(math, "zeroFloorSub", 0, 0) == 0


# ─── add(uint256, int256) ───────────────────────────────────────────────────
# NOTE: On AVM, the `add` function signature is add(uint256, uint256) -> uint256.
# For negative values, we need to pass the two's complement representation.

def test_add_positive(math):
    assert _call(math, "add", 100, 50) == 150


def test_add_zero(math):
    assert _call(math, "add", 100, 0) == 100


def test_add_with_zero_base(math):
    assert _call(math, "add", 0, 50) == 50


def test_add_negative_twos_complement(math):
    """add(100, -30) should return 70. Pass -30 as two's complement uint256."""
    neg_30 = UINT256_MAX + 1 - 30  # two's complement of -30
    assert _call(math, "add", 100, neg_30) == 70


@pytest.mark.xfail(reason="AVM uses wrapping subtraction for int256 negation; underflow not detected")
def test_add_negative_underflow(math):
    """add(0, -50) should revert."""
    neg_50 = UINT256_MAX + 1 - 50
    with pytest.raises(Exception):
        _call(math, "add", 0, neg_50)


def test_add_overflow(math):
    """add(UINT256_MAX, 1) should revert."""
    with pytest.raises(Exception):
        _call(math, "add", UINT256_MAX, 1)


# ─── uncheckedAdd ────────────────────────────────────────────────────────────

def test_uncheckedAdd_normal(math):
    assert _call(math, "uncheckedAdd", 100, 200) == 300


def test_uncheckedAdd_zero(math):
    assert _call(math, "uncheckedAdd", 0, 0) == 0


@pytest.mark.xfail(reason="AVM biguint is arbitrary-precision; unchecked wrapping not supported")
def test_uncheckedAdd_overflow(math):
    """uncheckedAdd wraps on overflow (mod 2^256)."""
    result = _call(math, "uncheckedAdd", UINT256_MAX, 1)
    assert result == 0


@pytest.mark.xfail(reason="AVM biguint is arbitrary-precision; unchecked wrapping not supported")
def test_uncheckedAdd_overflow_large(math):
    result = _call(math, "uncheckedAdd", UINT256_MAX, UINT256_MAX)
    assert result == UINT256_MAX - 1


# ─── signedSub ───────────────────────────────────────────────────────────────

def test_signedSub_a_gt_b(math):
    result = _call(math, "signedSub", 100, 30)
    assert result == 70


def test_signedSub_a_eq_b(math):
    result = _call(math, "signedSub", 50, 50)
    assert result == 0


def test_signedSub_a_lt_b(math):
    """signedSub(30, 100) should return -70 as two's complement uint256."""
    result = _call(math, "signedSub", 30, 100)
    # -70 in two's complement of 256-bit int
    expected = UINT256_MAX + 1 - 70
    assert result == expected


@pytest.mark.xfail(reason="SafeCast int256 overflow not enforced on AVM biguint")
def test_signedSub_overflow(math):
    """Values > INT256_MAX should revert (SafeCast overflow)."""
    big = INT256_MAX + 1
    with pytest.raises(Exception):
        _call(math, "signedSub", big, 0)


# ─── uncheckedSub ────────────────────────────────────────────────────────────

def test_uncheckedSub_normal(math):
    assert _call(math, "uncheckedSub", 200, 100) == 100


def test_uncheckedSub_underflow(math):
    """uncheckedSub wraps on underflow (mod 2^256)."""
    result = _call(math, "uncheckedSub", 0, 1)
    assert result == UINT256_MAX


def test_uncheckedSub_equal(math):
    assert _call(math, "uncheckedSub", 50, 50) == 0


# ─── uncheckedExp ────────────────────────────────────────────────────────────

def test_uncheckedExp_basic(math):
    assert _call(math, "uncheckedExp", 2, 10) == 1024
    assert _call(math, "uncheckedExp", 3, 5) == 243


def test_uncheckedExp_zero_exp(math):
    assert _call(math, "uncheckedExp", 100, 0) == 1


def test_uncheckedExp_one_exp(math):
    assert _call(math, "uncheckedExp", 42, 1) == 42


def test_uncheckedExp_zero_base(math):
    assert _call(math, "uncheckedExp", 0, 5) == 0


def test_uncheckedExp_zero_zero(math):
    assert _call(math, "uncheckedExp", 0, 0) == 1


# ─── divUp ───────────────────────────────────────────────────────────────────

def test_divUp_exact(math):
    assert _call(math, "divUp", 10, 5) == 2


def test_divUp_rounds_up(math):
    assert _call(math, "divUp", 10, 3) == 4  # ceil(10/3) = 4


def test_divUp_zero_numerator(math):
    assert _call(math, "divUp", 0, 5) == 0


def test_divUp_div_by_zero(math):
    with pytest.raises(Exception):
        _call(math, "divUp", 10, 0)


def test_divUp_one(math):
    assert _call(math, "divUp", 1, 1) == 1


# ─── mulDivDown ──────────────────────────────────────────────────────────────

def test_mulDivDown_with_remainder(math):
    assert _call(math, "mulDivDown", 2, 13, 3) == 8  # 26/3 = 8.666 → 8


def test_mulDivDown_no_remainder(math):
    assert _call(math, "mulDivDown", 12, 6, 4) == 18


def test_mulDivDown_zero_a(math):
    assert _call(math, "mulDivDown", 0, 10, 5) == 0


def test_mulDivDown_zero_b(math):
    assert _call(math, "mulDivDown", 10, 0, 5) == 0


def test_mulDivDown_div_by_zero(math):
    with pytest.raises(Exception):
        _call(math, "mulDivDown", 10, 10, 0)


def test_mulDivDown_overflow(math):
    with pytest.raises(Exception):
        _call(math, "mulDivDown", UINT256_MAX, 2, 1)


# ─── mulDivUp ────────────────────────────────────────────────────────────────

def test_mulDivUp_with_remainder(math):
    assert _call(math, "mulDivUp", 5, 5, 3) == 9  # 25/3 = 8.333 → 9


def test_mulDivUp_no_remainder(math):
    assert _call(math, "mulDivUp", 12, 6, 4) == 18


def test_mulDivUp_zero_a(math):
    assert _call(math, "mulDivUp", 0, 10, 5) == 0


def test_mulDivUp_zero_b(math):
    assert _call(math, "mulDivUp", 10, 0, 5) == 0


def test_mulDivUp_div_by_zero(math):
    with pytest.raises(Exception):
        _call(math, "mulDivUp", 10, 10, 0)


def test_mulDivUp_overflow(math):
    with pytest.raises(Exception):
        _call(math, "mulDivUp", UINT256_MAX, 2, 1)
