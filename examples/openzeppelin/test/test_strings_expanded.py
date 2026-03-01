"""
StringsExpanded behavioral tests.
Tests uint256/int256/address to string/hex conversion, plus string equality.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def strings(localnet, account):
    return deploy_contract(localnet, account, "StringsExpandedTest")


def test_deploy(strings):
    assert strings.app_id > 0


# --- toString ---

def test_to_string_zero(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toString", args=[0])
    )
    assert result.abi_return == "0"


def test_to_string_one(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toString", args=[1])
    )
    assert result.abi_return == "1"


def test_to_string_42(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toString", args=[42])
    )
    assert result.abi_return == "42"


def test_to_string_large(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toString", args=[1234567890])
    )
    assert result.abi_return == "1234567890"


def test_to_string_max_uint64(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="toString",
            args=[2**64 - 1],
        )
    )
    assert result.abi_return == str(2**64 - 1)


# --- toHexString ---

def test_to_hex_string_zero(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toHexString", args=[0])
    )
    assert result.abi_return == "0x00"


def test_to_hex_string_255(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toHexString", args=[255])
    )
    assert result.abi_return == "0xff"


def test_to_hex_string_256(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(method="toHexString", args=[256])
    )
    assert result.abi_return == "0x0100"


def test_to_hex_string_fixed(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="toHexStringFixed",
            args=[255, 4],
        )
    )
    assert result.abi_return == "0x000000ff"


# --- equal ---

def test_equal_same(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="equal",
            args=["hello", "hello"],
        )
    )
    assert result.abi_return is True


def test_equal_different(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="equal",
            args=["hello", "world"],
        )
    )
    assert result.abi_return is False


def test_equal_empty(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="equal",
            args=["", ""],
        )
    )
    assert result.abi_return is True


def test_equal_diff_length(strings):
    result = strings.send.call(
        au.AppClientMethodCallParams(
            method="equal",
            args=["ab", "abc"],
        )
    )
    assert result.abi_return is False
