"""
M2: Library functions with bytes/string ops.
Verifies BytesLib.asciiDigitToUint and BytesLib.concatBytes.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

ASCII_DIGIT_VECTORS = [
    (48, 0),   # '0'
    (49, 1),   # '1'
    (57, 9),   # '9'
    (50, 2),   # '2'
]

ASCII_DIGIT_REVERT = [
    65,   # 'A' — not a digit
    47,   # '/' — just below '0'
    58,   # ':' — just above '9'
]

CONCAT_VECTORS = [
    (b"\xde\xad", b"\xbe\xef", b"\xde\xad\xbe\xef"),
    (b"", b"\xab", b"\xab"),
    (b"\x01\x02", b"\x03\x04", b"\x01\x02\x03\x04"),
]


@pytest.fixture(scope="module")
def bytes_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "BytesLibTest")


@pytest.mark.localnet
@pytest.mark.parametrize("code,expected", ASCII_DIGIT_VECTORS)
def test_ascii_digit(bytes_client: au.AppClient, code: int, expected: int) -> None:
    result = bytes_client.send.call(
        au.AppClientMethodCallParams(method="testAsciiDigit", args=[code])
    )
    assert result.abi_return == expected


@pytest.mark.localnet
@pytest.mark.parametrize("code", ASCII_DIGIT_REVERT)
def test_ascii_digit_reverts(bytes_client: au.AppClient, code: int) -> None:
    with pytest.raises(Exception):
        bytes_client.send.call(
            au.AppClientMethodCallParams(method="testAsciiDigit", args=[code])
        )


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", CONCAT_VECTORS)
def test_concat(bytes_client: au.AppClient, a: bytes, b: bytes, expected: bytes) -> None:
    result = bytes_client.send.call(
        au.AppClientMethodCallParams(method="testConcat", args=[a, b])
    )
    ret = result.abi_return
    if isinstance(ret, list):
        ret = bytes(ret)
    assert ret == expected
