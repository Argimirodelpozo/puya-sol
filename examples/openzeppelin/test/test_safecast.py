"""
OpenZeppelin SafeCast library behavioral tests.
Tests safe downcasting with overflow checks.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def safecast(localnet, account):
    return deploy_contract(localnet, account, "SafeCastTest")


def test_deploy(safecast):
    assert safecast.app_id > 0


def test_to_uint128_valid(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint128", args=[12345])
    )
    assert result.abi_return == 12345


def test_to_uint128_max(safecast):
    max128 = 2**128 - 1
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint128", args=[max128])
    )
    assert result.abi_return == max128


def test_to_uint128_overflow(safecast):
    with pytest.raises(Exception):
        safecast.send.call(
            au.AppClientMethodCallParams(method="testToUint128", args=[2**128])
        )


def test_to_uint64_valid(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint64", args=[999])
    )
    assert result.abi_return == 999


def test_to_uint64_max(safecast):
    max64 = 2**64 - 1
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint64", args=[max64])
    )
    assert result.abi_return == max64


def test_to_uint64_overflow(safecast):
    with pytest.raises(Exception):
        safecast.send.call(
            au.AppClientMethodCallParams(method="testToUint64", args=[2**64])
        )


def test_to_uint32_valid(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint32", args=[100000])
    )
    assert result.abi_return == 100000


def test_to_uint32_overflow(safecast):
    with pytest.raises(Exception):
        safecast.send.call(
            au.AppClientMethodCallParams(method="testToUint32", args=[2**32])
        )


def test_to_uint16_valid(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint16", args=[65535])
    )
    assert result.abi_return == 65535


def test_to_uint16_overflow(safecast):
    with pytest.raises(Exception):
        safecast.send.call(
            au.AppClientMethodCallParams(method="testToUint16", args=[65536])
        )


def test_to_uint8_valid(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint8", args=[255])
    )
    assert result.abi_return == 255


def test_to_uint8_overflow(safecast):
    with pytest.raises(Exception):
        safecast.send.call(
            au.AppClientMethodCallParams(method="testToUint8", args=[256])
        )


def test_to_uint8_zero(safecast):
    result = safecast.send.call(
        au.AppClientMethodCallParams(method="testToUint8", args=[0])
    )
    assert result.abi_return == 0
