"""
OpenZeppelin ShortStrings behavioral tests.
Tests encoding/decoding strings <= 31 bytes into bytes32.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def short_strings(localnet, account):
    return deploy_contract(localnet, account, "ShortStringsTest")


def abi_bytes(ret) -> bytes:
    """Convert ABI return (list of ints for byte[N]) to bytes."""
    if isinstance(ret, (list, tuple)):
        return bytes(ret)
    return ret


def test_deploy(short_strings):
    assert short_strings.app_id > 0


def test_round_trip_empty(short_strings):
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="roundTrip",
            args=[""],
        )
    )
    assert result.abi_return == ""


def test_round_trip_hello(short_strings):
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="roundTrip",
            args=["hello"],
        )
    )
    assert result.abi_return == "hello"


def test_round_trip_max_length(short_strings):
    """31 characters is the maximum for ShortString."""
    max_str = "a" * 31
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="roundTrip",
            args=[max_str],
        )
    )
    assert result.abi_return == max_str


def test_byte_length(short_strings):
    # First encode a string, then check its byte length
    encoded = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="toShortString",
            args=["hello"],
        )
    )
    encoded_bytes = abi_bytes(encoded.abi_return)
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="byteLength",
            args=[encoded_bytes],
        )
    )
    assert result.abi_return == 5


def test_to_short_string(short_strings):
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="toShortString",
            args=["hello"],
        )
    )
    encoded = abi_bytes(result.abi_return)
    # Encoded bytes32 should be non-zero
    assert encoded != b"\x00" * 32


def test_to_string(short_strings):
    # Encode then decode via separate calls
    encoded = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="toShortString",
            args=["Algorand"],
        )
    )
    encoded_bytes = abi_bytes(encoded.abi_return)
    result = short_strings.send.call(
        au.AppClientMethodCallParams(
            method="toString",
            args=[encoded_bytes],
        )
    )
    assert result.abi_return == "Algorand"
