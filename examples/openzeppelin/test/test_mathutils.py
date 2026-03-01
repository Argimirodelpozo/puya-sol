"""
MathUtils library behavioral tests.
Tests max, min, average, ceilDiv, mulDiv, sqrt, log2, log10.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def mathutils(localnet, account):
    return deploy_contract(localnet, account, "MathUtilsTest")


def test_deploy(mathutils):
    assert mathutils.app_id > 0


def test_max(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testMax", args=[7, 3])
    )
    assert result.abi_return == 7


def test_min(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testMin", args=[7, 3])
    )
    assert result.abi_return == 3


def test_average(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testAverage", args=[10, 20])
    )
    assert result.abi_return == 15


def test_average_large_no_overflow(mathutils):
    """Average of two large numbers should not overflow on AVM."""
    big = 10**38
    result = mathutils.send.call(
        au.AppClientMethodCallParams(
            method="testAverage", args=[big, big + 2]
        )
    )
    assert result.abi_return == big + 1


def test_ceil_div(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testCeilDiv", args=[7, 3])
    )
    assert result.abi_return == 3  # ceil(7/3) = 3


def test_mul_div(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testMulDiv", args=[6, 7, 3])
    )
    assert result.abi_return == 14


def test_sqrt(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[100])
    )
    assert result.abi_return == 10


def test_sqrt_large(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testSqrt", args=[10**20])
    )
    assert result.abi_return == 10**10


def test_log2(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testLog2", args=[1024])
    )
    assert result.abi_return == 10


def test_log10(mathutils):
    result = mathutils.send.call(
        au.AppClientMethodCallParams(method="testLog10", args=[10000])
    )
    assert result.abi_return == 4
