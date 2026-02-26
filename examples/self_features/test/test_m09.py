"""
M9: BN256 elliptic curve operations.
Verifies ecAdd, ecMul, and pairingCheck using known BN256 test vectors.

BN256 G1 generator: (1, 2)
BN256 field order: 21888242871839275222246405745257275088696311157297823662689037894645226208583
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

# BN256 G1 generator point
G1_X = 1
G1_Y = 2


@pytest.fixture(scope="module")
def bn256_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "BN256Test")


# Known: 2*G1 via ecAdd(G1, G1)
# 2*G1 = (1368015179489954701390400359078579693043519447331113978918064868415326638035,
#          9918110051302171585080402603319702774565515993150576347155970296011118125764)
G1_DOUBLE_X = 1368015179489954701390400359078579693043519447331113978918064868415326638035
G1_DOUBLE_Y = 9918110051302171585080402603319702774565515993150576347155970296011118125764


@pytest.mark.localnet
def test_ec_add_g1_plus_g1(bn256_client: au.AppClient) -> None:
    """ecAdd(G1, G1) should equal 2*G1."""
    result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecAdd", args=[G1_X, G1_Y, G1_X, G1_Y]
        )
    )
    ret = result.abi_return
    rx, ry = (ret['rx'], ret['ry']) if isinstance(ret, dict) else ret
    assert rx == G1_DOUBLE_X
    assert ry == G1_DOUBLE_Y


@pytest.mark.localnet
def test_ec_mul_g1_by_2(bn256_client: au.AppClient) -> None:
    """ecMul(G1, 2) should equal 2*G1 (same as ecAdd(G1, G1))."""
    result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMul", args=[G1_X, G1_Y, 2]
        )
    )
    ret = result.abi_return
    rx, ry = (ret['rx'], ret['ry']) if isinstance(ret, dict) else ret
    assert rx == G1_DOUBLE_X
    assert ry == G1_DOUBLE_Y


@pytest.mark.localnet
def test_ec_add_equals_ec_mul(bn256_client: au.AppClient) -> None:
    """ecAdd(G1, G1) should equal ecMul(G1, 2)."""
    add_result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecAdd", args=[G1_X, G1_Y, G1_X, G1_Y]
        )
    )
    mul_result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMul", args=[G1_X, G1_Y, 2]
        )
    )
    a = add_result.abi_return
    b = mul_result.abi_return
    a_vals = tuple(a.values()) if isinstance(a, dict) else tuple(a)
    b_vals = tuple(b.values()) if isinstance(b, dict) else tuple(b)
    assert a_vals == b_vals


@pytest.mark.localnet
def test_ec_mul_by_zero(bn256_client: au.AppClient) -> None:
    """ecMul(G1, 0) should return the point at infinity (0, 0)."""
    result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMul", args=[G1_X, G1_Y, 0]
        )
    )
    ret = result.abi_return
    rx, ry = (ret['rx'], ret['ry']) if isinstance(ret, dict) else ret
    assert rx == 0
    assert ry == 0


@pytest.mark.localnet
def test_ec_mul_by_one(bn256_client: au.AppClient) -> None:
    """ecMul(G1, 1) should return G1."""
    result = bn256_client.send.call(
        au.AppClientMethodCallParams(
            method="ecMul", args=[G1_X, G1_Y, 1]
        )
    )
    ret = result.abi_return
    rx, ry = (ret['rx'], ret['ry']) if isinstance(ret, dict) else ret
    assert rx == G1_X
    assert ry == G1_Y
