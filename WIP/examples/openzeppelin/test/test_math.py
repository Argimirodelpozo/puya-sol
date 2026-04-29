"""
OpenZeppelin Math library behavioral tests.
Tests mulDiv, sqrt, log2, log10, ceilDiv, average, max, min.
"""

import math
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def math_contract(localnet, account):
    return deploy_contract(localnet, account, "MathTest")


def test_deploy(math_contract):
    assert math_contract.app_id > 0


def test_max_first_larger(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMax", args=[100, 50])
    )
    assert result.abi_return == 100


def test_max_second_larger(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMax", args=[50, 100])
    )
    assert result.abi_return == 100


def test_max_equal(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMax", args=[42, 42])
    )
    assert result.abi_return == 42


def test_min_first_smaller(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMin", args=[10, 50])
    )
    assert result.abi_return == 10


def test_min_second_smaller(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMin", args=[50, 10])
    )
    assert result.abi_return == 10


def test_average_even(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testAverage", args=[10, 20])
    )
    assert result.abi_return == 15


def test_average_odd(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testAverage", args=[10, 21])
    )
    assert result.abi_return == 15  # floor


def test_average_zero(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testAverage", args=[0, 0])
    )
    assert result.abi_return == 0


def test_ceil_div(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testCeilDiv", args=[10, 3])
    )
    assert result.abi_return == 4  # ceil(10/3) = 4


def test_ceil_div_exact(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testCeilDiv", args=[9, 3])
    )
    assert result.abi_return == 3


def test_ceil_div_zero_numerator(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testCeilDiv", args=[0, 5])
    )
    assert result.abi_return == 0


def test_mul_div(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testMulDiv", args=[100, 200, 50])
    )
    assert result.abi_return == 400


def test_mul_div_large(math_contract):
    """Test with large numbers that would overflow uint256 in EVM."""
    big = 10**38
    result = math_contract.send.call(
        au.AppClientMethodCallParams(
            method="testMulDiv", args=[big, big, big]
        )
    )
    assert result.abi_return == big


def test_sqrt_perfect(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[144])
    )
    assert result.abi_return == 12


def test_sqrt_non_perfect(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[200])
    )
    assert result.abi_return == 14  # floor(sqrt(200)) = 14


def test_sqrt_zero(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[0])
    )
    assert result.abi_return == 0


def test_sqrt_one(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[1])
    )
    assert result.abi_return == 1


def test_sqrt_large(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[10**20])
    )
    assert result.abi_return == 10**10


def test_log2_power_of_two(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog2", args=[256])
    )
    assert result.abi_return == 8


def test_log2_non_power(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog2", args=[255])
    )
    assert result.abi_return == 7  # floor(log2(255)) = 7


def test_log2_one(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog2", args=[1])
    )
    assert result.abi_return == 0


def test_log10_power_of_ten(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog10", args=[1000])
    )
    assert result.abi_return == 3


def test_log10_non_power(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog10", args=[999])
    )
    assert result.abi_return == 2  # floor(log10(999)) = 2


def test_log10_one(math_contract):
    result = math_contract.send.call(
        au.AppClientMethodCallParams(method="testLog10", args=[1])
    )
    assert result.abi_return == 0
