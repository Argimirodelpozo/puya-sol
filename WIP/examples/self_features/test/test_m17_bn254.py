"""
M17 BN254 basic operations test.
Tests ec_add and ec_scalar_mul with the BN254Test contract.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

# BN254 G1 generator
G1_X = 1
G1_Y = 2

# Known 2*G1 coordinates (point doubling of the generator)
G1_2X = 1368015179489954701390400359078579693043519447331113978918064868415326638035
G1_2Y = 9918110051302171585080402603319702774565515993150576347155970296011118125764

# BN254 base field modulus
Q = 21888242871839275222246405745257275088696311157297823662689037894645226208583


@pytest.fixture(scope="module")
def bn254_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "BN254Test")


@pytest.mark.localnet
def test_ec_add_generator_doubling(bn254_client: au.AppClient) -> None:
    """ec_add(G1, G1) should give 2*G1."""
    result = bn254_client.send.call(
        au.AppClientMethodCallParams(
            method="ecAddX",
            args=[G1_X, G1_Y, G1_X, G1_Y],
        )
    )
    assert result.abi_return == G1_2X


@pytest.mark.localnet
def test_ec_mul_by_2(bn254_client: au.AppClient) -> None:
    """ec_mul(G1, 2) should give 2*G1 (same x as ec_add(G1, G1))."""
    result = bn254_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMulX",
            args=[G1_X, G1_Y, 2],
        )
    )
    assert result.abi_return == G1_2X


@pytest.mark.localnet
def test_ec_mul_by_1(bn254_client: au.AppClient) -> None:
    """ec_mul(G1, 1) should give G1."""
    result = bn254_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMulX",
            args=[G1_X, G1_Y, 1],
        )
    )
    assert result.abi_return == G1_X


@pytest.mark.localnet
def test_negate_y(bn254_client: au.AppClient) -> None:
    """negateY(2) should give q - 2."""
    result = bn254_client.send.call(
        au.AppClientMethodCallParams(
            method="negateY",
            args=[G1_Y],
        )
    )
    assert result.abi_return == Q - G1_Y
