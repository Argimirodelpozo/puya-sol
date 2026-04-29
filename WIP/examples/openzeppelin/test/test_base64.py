"""
Base64 behavioral tests.
Tests Base64 encoding (pure function).
"""

import base64
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def b64(localnet, account):
    return deploy_contract(localnet, account, "Base64Test")


def test_deploy(b64):
    assert b64.app_id > 0


def test_encode_empty(b64):
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[b""],
        )
    )
    assert result.abi_return == ""


def test_encode_hello(b64):
    data = b"Hello, World!"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_single_byte(b64):
    data = b"A"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_two_bytes(b64):
    data = b"AB"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_three_bytes(b64):
    """Three bytes = no padding."""
    data = b"ABC"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_algorand(b64):
    data = b"Algorand"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_zeros(b64):
    data = b"\x00\x00\x00"
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
            note=b"zeros",
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected


def test_encode_binary(b64):
    """Encode 0x00-0xFF."""
    data = bytes(range(256))
    result = b64.send.call(
        au.AppClientMethodCallParams(
            method="encode",
            args=[data],
        )
    )
    expected = base64.b64encode(data).decode()
    assert result.abi_return == expected
