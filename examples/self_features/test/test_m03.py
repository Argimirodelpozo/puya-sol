"""
M3: Struct creation, return, and field access (pure).
Verifies Point struct creation and field reads.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract

MAKE_POINT_VECTORS = [
    (3, 4, (3, 4)),
    (0, 0, (0, 0)),
    (100, 200, (100, 200)),
]

ADD_POINTS_VECTORS = [
    # (x1, y1, x2, y2, expected_x, expected_y)
    (1, 2, 3, 4, 4, 6),
    (0, 0, 5, 10, 5, 10),
    (100, 200, 300, 400, 400, 600),
]


@pytest.fixture(scope="module")
def struct_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "StructTest")


@pytest.mark.localnet
@pytest.mark.parametrize("x,y,expected", MAKE_POINT_VECTORS)
def test_make_point(struct_client: au.AppClient, x: int, y: int, expected: tuple) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(method="makePoint", args=[x, y])
    )
    ret = result.abi_return
    vals = tuple(ret.values()) if isinstance(ret, dict) else tuple(ret)
    assert vals == expected


@pytest.mark.localnet
@pytest.mark.parametrize("x1,y1,x2,y2,ex,ey", ADD_POINTS_VECTORS)
def test_add_points(
    struct_client: au.AppClient,
    x1: int, y1: int, x2: int, y2: int, ex: int, ey: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(method="addPoints", args=[x1, y1, x2, y2])
    )
    ret = result.abi_return
    vals = tuple(ret.values()) if isinstance(ret, dict) else tuple(ret)
    assert vals == (ex, ey)
