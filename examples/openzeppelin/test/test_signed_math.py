"""
OpenZeppelin SignedMath library behavioral tests.
Tests max, min, average, abs for signed int256 values.

int256 two's complement encoding for ABI (biguint):
  - Positive N -> N
  - Negative N -> 2**256 + N  (i.e. 2**256 - |N|)
  - Min int256 = 2**255
  - Max int256 = 2**255 - 1
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

TWO_256 = 2**256
TWO_255 = 2**255
MAX_INT256 = TWO_255 - 1   # 0x7FFF...FFFF
MIN_INT256 = TWO_255        # 0x8000...0000 (two's complement of -2^255)


def encode_int256(n: int) -> int:
    """Encode a Python signed integer as a two's complement uint256 for ABI."""
    if n >= 0:
        return n
    return TWO_256 + n


def decode_int256(val: int) -> int:
    """Decode a two's complement uint256 ABI value back to a Python signed integer."""
    if val >= TWO_255:
        return val - TWO_256
    return val


@pytest.fixture(scope="module")
def signed_math(localnet, account):
    return deploy_contract(localnet, account, "SignedMathTest")


def test_deploy(signed_math):
    assert signed_math.app_id > 0


# --- max ---

def test_max_both_positive(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(100), encode_int256(50)],
        )
    )
    assert decode_int256(result.abi_return) == 100


def test_max_both_positive_second_larger(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(10), encode_int256(200)],
        )
    )
    assert decode_int256(result.abi_return) == 200


def test_max_one_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(-5), encode_int256(10)],
        )
    )
    assert decode_int256(result.abi_return) == 10


def test_max_both_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(-100), encode_int256(-3)],
        )
    )
    assert decode_int256(result.abi_return) == -3


def test_max_equal(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(42), encode_int256(42)],
        )
    )
    assert decode_int256(result.abi_return) == 42


def test_max_zero_and_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[encode_int256(0), encode_int256(-1)],
        )
    )
    assert decode_int256(result.abi_return) == 0


# --- min ---

def test_min_both_positive(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[encode_int256(100), encode_int256(50)],
        )
    )
    assert decode_int256(result.abi_return) == 50


def test_min_one_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[encode_int256(-5), encode_int256(10)],
        )
    )
    assert decode_int256(result.abi_return) == -5


def test_min_both_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[encode_int256(-3), encode_int256(-100)],
        )
    )
    assert decode_int256(result.abi_return) == -100


def test_min_zero_and_positive(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[encode_int256(0), encode_int256(7)],
        )
    )
    assert decode_int256(result.abi_return) == 0


# --- average ---

def test_average_two_positive(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(10), encode_int256(20)],
        )
    )
    assert decode_int256(result.abi_return) == 15


def test_average_two_positive_odd(signed_math):
    """Average of 10 and 21 should round towards zero -> 15."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(10), encode_int256(21)],
        )
    )
    assert decode_int256(result.abi_return) == 15


def test_average_positive_and_negative(signed_math):
    """Average of -10 and 10 should be 0."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(-10), encode_int256(10)],
        )
    )
    assert decode_int256(result.abi_return) == 0


def test_average_positive_and_negative_unequal(signed_math):
    """Average of -3 and 7 should be 2 (rounds towards zero)."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(-3), encode_int256(7)],
        )
    )
    assert decode_int256(result.abi_return) == 2


def test_average_both_negative(signed_math):
    """Average of -10 and -20 should be -15."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(-10), encode_int256(-20)],
        )
    )
    assert decode_int256(result.abi_return) == -15


def test_average_zeros(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAverage",
            args=[encode_int256(0), encode_int256(0)],
        )
    )
    assert decode_int256(result.abi_return) == 0


# --- abs ---

def test_abs_positive(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(42)],
        )
    )
    assert result.abi_return == 42


def test_abs_negative(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(-42)],
        )
    )
    assert result.abi_return == 42


def test_abs_zero(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(0)],
        )
    )
    assert result.abi_return == 0


def test_abs_negative_one(signed_math):
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(-1)],
        )
    )
    assert result.abi_return == 1


def test_abs_large_positive(signed_math):
    val = 10**38
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(val)],
        )
    )
    assert result.abi_return == val


def test_abs_large_negative(signed_math):
    val = -(10**38)
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[encode_int256(val)],
        )
    )
    assert result.abi_return == 10**38


# --- edge cases: max int256 and min int256 ---

def test_max_with_max_int256(signed_math):
    """max(MAX_INT256, 0) should return MAX_INT256."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[MAX_INT256, encode_int256(0)],
        )
    )
    assert decode_int256(result.abi_return) == MAX_INT256


def test_min_with_min_int256(signed_math):
    """min(MIN_INT256, 0) should return MIN_INT256 (most negative value)."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[MIN_INT256, encode_int256(0)],
        )
    )
    assert decode_int256(result.abi_return) == -TWO_255


def test_max_max_int256_vs_min_int256(signed_math):
    """max(MAX_INT256, MIN_INT256) should return MAX_INT256."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMax",
            args=[MAX_INT256, MIN_INT256],
        )
    )
    assert decode_int256(result.abi_return) == MAX_INT256


def test_min_max_int256_vs_min_int256(signed_math):
    """min(MAX_INT256, MIN_INT256) should return MIN_INT256."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testMin",
            args=[MAX_INT256, MIN_INT256],
        )
    )
    assert decode_int256(result.abi_return) == -TWO_255


def test_abs_max_int256(signed_math):
    """abs(MAX_INT256) should return MAX_INT256."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[MAX_INT256],
        )
    )
    assert result.abi_return == MAX_INT256


def test_abs_min_int256(signed_math):
    """abs(MIN_INT256) = abs(-2^255) = 2^255."""
    result = signed_math.send.call(
        au.AppClientMethodCallParams(
            method="testAbs",
            args=[MIN_INT256],
        )
    )
    assert result.abi_return == TWO_255
