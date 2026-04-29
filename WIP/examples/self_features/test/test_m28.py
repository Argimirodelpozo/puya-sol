"""
M28: Modular exponentiation via modexp precompile (0x05).
Tests base^exponent % modulus using the square-and-multiply loop.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def modexp_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "ModExpTest")


MODEXP_VECTORS = [
    # (base, exponent, modulus, expected)
    # Basic cases
    (2, 10, 1000, 1024 % 1000),           # 2^10 % 1000 = 24
    (3, 5, 13, pow(3, 5, 13)),             # 3^5 % 13 = 9
    (2, 256, 97, pow(2, 256, 97)),         # large exponent, small modulus
    (7, 0, 13, 1),                         # anything^0 = 1
    (0, 5, 7, 0),                          # 0^5 = 0
    (5, 1, 13, 5),                         # x^1 = x (when x < mod)
    (1, 1000, 7, 1),                       # 1^anything = 1
    # Modular arithmetic
    (2, 255, (1 << 256) - 1, pow(2, 255, (1 << 256) - 1)),  # large modulus
    (123456789, 987654321, 1000000007, pow(123456789, 987654321, 1000000007)),
    # RSA-style: moderately large numbers
    (65537, 3, 2**128 - 159, pow(65537, 3, 2**128 - 159)),
]


@pytest.mark.localnet
@pytest.mark.parametrize("base,exponent,modulus,expected", MODEXP_VECTORS)
def test_modexp(
    modexp_client: au.AppClient,
    base: int,
    exponent: int,
    modulus: int,
    expected: int,
) -> None:
    result = modexp_client.send.call(
        au.AppClientMethodCallParams(
            method="modexp(uint512,uint512,uint512)uint512",
            args=[base, exponent, modulus],
        )
    )
    assert result.abi_return == expected, (
        f"modexp({base}, {exponent}, {modulus}) = {result.abi_return}, expected {expected}"
    )
