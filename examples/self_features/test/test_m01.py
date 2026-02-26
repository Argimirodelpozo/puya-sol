"""
M1: Library function calls (pure math).
Verifies MathLib.square and MathLib.clamp work end-to-end.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

SQUARE_VECTORS = [
    (5, 25),
    (0, 0),
    (1, 1),
    (10, 100),
    (256, 65536),
]

CLAMP_VECTORS = [
    # (val, lo, hi, expected)
    (50, 10, 100, 50),   # in range
    (5, 10, 100, 10),    # below → lo
    (200, 10, 100, 100), # above → hi
    (10, 10, 100, 10),   # at lo boundary
    (100, 10, 100, 100), # at hi boundary
]


@pytest.fixture(scope="module")
def math_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "MathLibTest")


@pytest.mark.localnet
@pytest.mark.parametrize("x,expected", SQUARE_VECTORS)
def test_square(math_client: au.AppClient, x: int, expected: int) -> None:
    result = math_client.send.call(
        au.AppClientMethodCallParams(method="testSquare", args=[x])
    )
    assert result.abi_return == expected


@pytest.mark.localnet
@pytest.mark.parametrize("val,lo,hi,expected", CLAMP_VECTORS)
def test_clamp(
    math_client: au.AppClient, val: int, lo: int, hi: int, expected: int
) -> None:
    result = math_client.send.call(
        au.AppClientMethodCallParams(method="testClamp", args=[val, lo, hi])
    )
    assert result.abi_return == expected
