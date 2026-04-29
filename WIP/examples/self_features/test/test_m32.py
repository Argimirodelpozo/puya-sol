"""
M32: Advanced struct tests — nested structs, array members, field mutation,
struct pass/return through internal functions, multiple struct returns.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def struct_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "StructAdvancedTest")


# ─── Nested struct construction + access ─────────────────────────────

NESTED_VECTORS = [
    (1, 2, 3, 4),
    (0, 0, 0, 0),
    (2**128, 2**128, 2**64, 2**64),
    (42, 99, 7, 13),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x1,y1,x2,y2", NESTED_VECTORS)
def test_nested_struct(
    struct_client: au.AppClient, x1: int, y1: int, x2: int, y2: int
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testNestedStruct(uint512,uint512,uint512,uint512)(uint512,uint512,uint512,uint512)",
            args=[x1, y1, x2, y2],
        )
    )
    assert list(result.abi_return) == [x1, y1, x2, y2]


# ─── Struct with array member ────────────────────────────────────────

ARRAY_MEMBER_VECTORS = [
    (1, 10, 20, 30, 1, 20),
    (0, 0, 0, 0, 0, 0),
    (99, 2**128, 42, 7, 99, 42),
]


@pytest.mark.localnet
@pytest.mark.parametrize("id_,a,b,c,exp_id,exp_b", ARRAY_MEMBER_VECTORS)
def test_struct_with_array(
    struct_client: au.AppClient,
    id_: int, a: int, b: int, c: int, exp_id: int, exp_b: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testStructWithArray(uint512,uint512,uint512,uint512)(uint512,uint512)",
            args=[id_, a, b, c],
        )
    )
    assert list(result.abi_return) == [exp_id, exp_b]


# ─── Struct field mutation ───────────────────────────────────────────

MUTATION_VECTORS = [
    (10, 20, 99, 99, 20),
    (0, 0, 42, 42, 0),
    (2**128, 2**64, 1, 1, 2**64),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x,y,newX,expX,expY", MUTATION_VECTORS)
def test_field_mutation(
    struct_client: au.AppClient,
    x: int, y: int, newX: int, expX: int, expY: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testFieldMutation(uint512,uint512,uint512)(uint512,uint512)",
            args=[x, y, newX],
        )
    )
    assert list(result.abi_return) == [expX, expY]


# ─── Nested struct field mutation ────────────────────────────────────

NESTED_MUTATION_VECTORS = [
    (1, 2, 3, 4, 99, 99, 2),
    (0, 0, 0, 0, 42, 42, 0),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x1,y1,x2,y2,newX,expX,expY", NESTED_MUTATION_VECTORS)
def test_nested_field_mutation(
    struct_client: au.AppClient,
    x1: int, y1: int, x2: int, y2: int, newX: int, expX: int, expY: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testNestedFieldMutation(uint512,uint512,uint512,uint512,uint512)(uint512,uint512)",
            args=[x1, y1, x2, y2, newX],
        )
    )
    assert list(result.abi_return) == [expX, expY]


# ─── Struct pass/return through internal functions ───────────────────

PASS_RETURN_VECTORS = [
    (1, 2, 3, 4, 4, 6),
    (0, 0, 0, 0, 0, 0),
    (100, 200, 300, 400, 400, 600),
    (2**128, 2**64, 2**128, 2**64, 2**129, 2**65),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x1,y1,x2,y2,expX,expY", PASS_RETURN_VECTORS)
def test_struct_pass_return(
    struct_client: au.AppClient,
    x1: int, y1: int, x2: int, y2: int, expX: int, expY: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testStructPassReturn(uint512,uint512,uint512,uint512)(uint512,uint512)",
            args=[x1, y1, x2, y2],
        )
    )
    assert list(result.abi_return) == [expX, expY]


# ─── Multiple struct returns ─────────────────────────────────────────

@pytest.mark.localnet
@pytest.mark.parametrize("x1,y1,x2,y2", NESTED_VECTORS)
def test_multi_struct_return(
    struct_client: au.AppClient, x1: int, y1: int, x2: int, y2: int,
) -> None:
    result = struct_client.send.call(
        au.AppClientMethodCallParams(
            method="testMultiStructReturn(uint512,uint512,uint512,uint512)(uint512,uint512,uint512,uint512)",
            args=[x1, y1, x2, y2],
        )
    )
    assert list(result.abi_return) == [x1, y1, x2, y2]
