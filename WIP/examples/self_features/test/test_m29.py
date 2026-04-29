"""
M29: Feature tests — user-defined value types, operator overloading, enums, unchecked blocks.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def feature_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "FeatureTest")


# ─── User-Defined Value Type: wrap/unwrap ────────────────────────

WRAP_VECTORS = [0, 1, 42, 2**128, 2**256 - 1]


@pytest.mark.localnet
@pytest.mark.parametrize("x", WRAP_VECTORS)
def test_wrap_unwrap(feature_client: au.AppClient, x: int) -> None:
    """Fr.wrap(x) followed by Fr.unwrap should return x unchanged."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testWrap(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == x


# ─── Operator Overloading: arithmetic ────────────────────────────

FR_ADD_VECTORS = [
    (0, 0, 0),
    (1, 2, 3),
    (100, 200, 300),
    (2**128, 2**128, 2**129),
    (2**255, 2**255, 2**256),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", FR_ADD_VECTORS)
def test_fr_add(feature_client: au.AppClient, a: int, b: int, expected: int) -> None:
    """Test Fr + Fr operator overloading."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testFrAdd(uint512,uint512)uint512",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


FR_SUB_VECTORS = [
    (10, 3, 7),
    (100, 0, 100),
    (2**128, 1, 2**128 - 1),
    (2**256, 2**255, 2**255),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", FR_SUB_VECTORS)
def test_fr_sub(feature_client: au.AppClient, a: int, b: int, expected: int) -> None:
    """Test Fr - Fr operator overloading."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testFrSub(uint512,uint512)uint512",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


FR_MUL_VECTORS = [
    (0, 100, 0),
    (1, 42, 42),
    (7, 6, 42),
    (2**64, 2**64, 2**128),
    (2**128, 2, 2**129),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", FR_MUL_VECTORS)
def test_fr_mul(feature_client: au.AppClient, a: int, b: int, expected: int) -> None:
    """Test Fr * Fr operator overloading."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testFrMul(uint512,uint512)uint512",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


# ─── Operator Overloading: comparison ────────────────────────────

FR_EQ_VECTORS = [
    (0, 0, True),
    (42, 42, True),
    (1, 2, False),
    (2**256 - 1, 2**256 - 1, True),
    (2**256 - 1, 2**256 - 2, False),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", FR_EQ_VECTORS)
def test_fr_eq(feature_client: au.AppClient, a: int, b: int, expected: bool) -> None:
    """Test Fr == Fr operator overloading."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testFrEq(uint512,uint512)bool",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


FR_NEQ_VECTORS = [
    (0, 0, False),
    (1, 2, True),
    (42, 42, False),
    (2**255, 2**254, True),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", FR_NEQ_VECTORS)
def test_fr_neq(feature_client: au.AppClient, a: int, b: int, expected: bool) -> None:
    """Test Fr != Fr operator overloading."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testFrNeq(uint512,uint512)bool",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


# ─── Enum ────────────────────────────────────────────────────────

@pytest.mark.localnet
def test_enum_value(feature_client: au.AppClient) -> None:
    """Color.Blue should be ordinal 2."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testEnumValue()uint64",
        )
    )
    assert result.abi_return == 2


@pytest.mark.localnet
def test_enum_compare(feature_client: au.AppClient) -> None:
    """Color.Red != Color.Green should be true."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testEnumCompare()bool",
        )
    )
    assert result.abi_return is True


ENUM_INDEX_VECTORS = [
    (0, 0),   # Red
    (1, 1),   # Green
    (2, 2),   # Blue
    (3, 3),   # Yellow (default branch)
]


@pytest.mark.localnet
@pytest.mark.parametrize("idx,expected", ENUM_INDEX_VECTORS)
def test_enum_index(feature_client: au.AppClient, idx: int, expected: int) -> None:
    """Each enum member should map to its ordinal index."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testEnumIndex(uint64)uint64",
            args=[idx],
        )
    )
    assert result.abi_return == expected


# ─── Unchecked block ─────────────────────────────────────────────

UNCHECKED_VECTORS = [
    (0, 0, 0),
    (100, 200, 300),
    (2**128, 2**128, 2**129),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", UNCHECKED_VECTORS)
def test_unchecked(feature_client: au.AppClient, a: int, b: int, expected: int) -> None:
    """Unchecked block should compute addition correctly."""
    result = feature_client.send.call(
        au.AppClientMethodCallParams(
            method="testUnchecked(uint512,uint512)uint512",
            args=[a, b],
        )
    )
    assert result.abi_return == expected
