"""
M31: Easy wins — constant expressions, custom errors, using-for method calls,
type casts (bytes32↔uint256), bytes slicing/length, compile-time exponentiation.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


MODULUS = 21888242871839275222246405745257275088548364400416034343698204186575808495617


@pytest.fixture(scope="module")
def easy_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "EasyWinsTest")


# ─── Constant expression tests ──────────────────────────────────────

@pytest.mark.localnet
def test_const_modulus_minus_one(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testConstModulusMinusOne()uint512",
        )
    )
    assert result.abi_return == MODULUS - 1


@pytest.mark.localnet
def test_const_two_pow_128(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testConstTwoPow128()uint512",
        )
    )
    assert result.abi_return == 2**128


@pytest.mark.localnet
def test_const_two_pow_64(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testConstTwoPow64()uint512",
        )
    )
    assert result.abi_return == 2**64


@pytest.mark.localnet
def test_const_shift_14(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testConstShift14()uint512",
        )
    )
    assert result.abi_return == 1 << 14


@pytest.mark.localnet
def test_const_shift_68(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testConstShift68()uint512",
        )
    )
    assert result.abi_return == 1 << 68


# ─── Custom error tests ─────────────────────────────────────────────

REQUIRE_PASS_VECTORS = [1, 42, 100, 2**128, 2**256 - 1]


@pytest.mark.localnet
@pytest.mark.parametrize("x", REQUIRE_PASS_VECTORS)
def test_require_pass(easy_client: au.AppClient, x: int) -> None:
    """require(x > 0, Errors.InsufficientBalance()) should pass for x > 0."""
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testRequirePass(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == x


@pytest.mark.localnet
def test_require_fail_custom_error(easy_client: au.AppClient) -> None:
    """require(x > 0, Errors.InsufficientBalance()) should revert for x == 0."""
    with pytest.raises(Exception):
        easy_client.send.call(
            au.AppClientMethodCallParams(
                method="testRequirePass(uint512)uint512",
                args=[0],
            )
        )


@pytest.mark.localnet
@pytest.mark.parametrize("x", [1, 42, 999])
def test_require_with_error_pass(easy_client: au.AppClient, x: int) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testRequireWithError(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == x


@pytest.mark.localnet
def test_require_with_error_fail(easy_client: au.AppClient) -> None:
    """require(x < 1000, Errors.InvalidAmount(x)) should revert for x >= 1000."""
    with pytest.raises(Exception):
        easy_client.send.call(
            au.AppClientMethodCallParams(
                method="testRequireWithError(uint512)uint512",
                args=[1000],
            )
        )


# ─── Using-for method calls on UDVT ─────────────────────────────────

AMOUNT_ADD_VECTORS = [
    (0, 0, 0),
    (1, 2, 3),
    (100, 200, 300),
    (2**128, 2**128, 2**129),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,expected", AMOUNT_ADD_VECTORS)
def test_amount_add(easy_client: au.AppClient, a: int, b: int, expected: int) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testAmountAdd(uint512,uint512)uint512",
            args=[a, b],
        )
    )
    assert result.abi_return == expected


AMOUNT_IS_ZERO_VECTORS = [
    (0, True),
    (1, False),
    (42, False),
    (2**256 - 1, False),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x,expected", AMOUNT_IS_ZERO_VECTORS)
def test_amount_is_zero(easy_client: au.AppClient, x: int, expected: bool) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testAmountIsZero(uint512)bool",
            args=[x],
        )
    )
    assert result.abi_return == expected


# ─── bytes32 ↔ uint256 type cast ────────────────────────────────────

BYTES32_VECTORS = [0, 1, 42, 2**128, 2**256 - 1]


@pytest.mark.localnet
@pytest.mark.parametrize("x", BYTES32_VECTORS)
def test_bytes32_from_uint(easy_client: au.AppClient, x: int) -> None:
    """bytes32(x) should produce the big-endian 32-byte representation."""
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testBytes32FromUint(uint512)byte[32]",
            args=[x],
        )
    )
    expected = x.to_bytes(32, "big")
    assert bytes(result.abi_return) == expected


@pytest.mark.localnet
@pytest.mark.parametrize("x", BYTES32_VECTORS)
def test_uint_from_bytes32(easy_client: au.AppClient, x: int) -> None:
    """uint256(bytes32_val) should recover the original value."""
    b = x.to_bytes(32, "big")
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testUintFromBytes32(byte[32])uint512",
            args=[b],
        )
    )
    assert result.abi_return == x


# ─── bytes slicing + length ──────────────────────────────────────────

@pytest.mark.localnet
def test_bytes_slice_length_even(easy_client: au.AppClient) -> None:
    """data[0:len/2].length should be half the original length."""
    data = bytes(range(20))  # 20 bytes
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testBytesSliceAndLength(byte[])uint512",
            args=[data],
        )
    )
    assert result.abi_return == 10


@pytest.mark.localnet
def test_bytes_slice_length_odd(easy_client: au.AppClient) -> None:
    """data[0:len/2].length for odd length rounds down."""
    data = bytes(range(13))  # 13 bytes → 13/2 = 6
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testBytesSliceAndLength(byte[])uint512",
            args=[data],
        )
    )
    assert result.abi_return == 6


# ─── Compile-time exponentiation ─────────────────────────────────────

@pytest.mark.localnet
def test_inline_pow(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testInlinePow()uint512",
        )
    )
    assert result.abi_return == 1024


# ─── Revert with custom error ────────────────────────────────────────

@pytest.mark.localnet
def test_revert_custom_pass(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testRevertCustom(uint512)uint512",
            args=[42],
        )
    )
    assert result.abi_return == 42


@pytest.mark.localnet
def test_revert_custom_fail(easy_client: au.AppClient) -> None:
    with pytest.raises(Exception):
        easy_client.send.call(
            au.AppClientMethodCallParams(
                method="testRevertCustom(uint512)uint512",
                args=[0],
            )
        )


# ─── Combined: using-for + custom error ──────────────────────────────

@pytest.mark.localnet
def test_combined_pass(easy_client: au.AppClient) -> None:
    result = easy_client.send.call(
        au.AppClientMethodCallParams(
            method="testCombined(uint512,uint512)uint512",
            args=[10, 20],
        )
    )
    assert result.abi_return == 30


@pytest.mark.localnet
def test_combined_fail(easy_client: au.AppClient) -> None:
    with pytest.raises(Exception):
        easy_client.send.call(
            au.AppClientMethodCallParams(
                method="testCombined(uint512,uint512)uint512",
                args=[0, 20],
            )
        )
