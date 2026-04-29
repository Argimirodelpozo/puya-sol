"""
M30: Advanced features — abi.encodePacked with arrays, abstract/virtual/override,
external library functions, high-level .staticcall, inheritance specifier constructor args.
"""

import hashlib

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def advanced_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "AdvancedFeatureTest")


# ─── abi.encodePacked with static uint256 array ─────────────────

def _pack_uint256(*values: int) -> bytes:
    """Python reference: abi.encodePacked for uint256 values (32 bytes each)."""
    return b"".join(v.to_bytes(32, "big") for v in values)


def _keccak256(data: bytes) -> bytes:
    """Python keccak256 reference."""
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


ENCODE_PACKED_VECTORS = [
    (1, 2, 3),
    (0, 0, 0),
    (2**256 - 1, 2**256 - 1, 2**256 - 1),
    (42, 0, 99),
    (2**128, 2**128, 2**128),
]


@pytest.mark.localnet
@pytest.mark.parametrize("a,b,c", ENCODE_PACKED_VECTORS)
def test_encode_packed_uint_array(
    advanced_client: au.AppClient, a: int, b: int, c: int
) -> None:
    """keccak256(abi.encodePacked([a, b, c])) should match Python reference."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="testEncodePackedUintArray(uint512,uint512,uint512)byte[32]",
            args=[a, b, c],
        )
    )
    expected = _keccak256(_pack_uint256(a, b, c))
    assert bytes(result.abi_return) == expected


# ─── abi.encodePacked with mixed scalar + array ─────────────────

ENCODE_PACKED_MIXED_VECTORS = [
    (100, 10, 20),
    (0, 0, 0),
    (2**255, 1, 2),
]


@pytest.mark.localnet
@pytest.mark.parametrize("prefix,x,y", ENCODE_PACKED_MIXED_VECTORS)
def test_encode_packed_mixed(
    advanced_client: au.AppClient, prefix: int, x: int, y: int
) -> None:
    """keccak256(abi.encodePacked(prefix, [x, y])) should match reference."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="testEncodePackedMixed(uint512,uint512,uint512)byte[32]",
            args=[prefix, x, y],
        )
    )
    expected = _keccak256(_pack_uint256(prefix, x, y))
    assert bytes(result.abi_return) == expected


# ─── External library function ───────────────────────────────────

EXTERNAL_LIB_VECTORS = [
    (0, 0),
    (1, 1),
    (7, 49),
    (10, 100),
    (2**64, 2**128),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x,expected", EXTERNAL_LIB_VECTORS)
def test_external_lib(
    advanced_client: au.AppClient, x: int, expected: int
) -> None:
    """MathLib.square(x) should return x*x."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="testExternalLib(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == expected


# ─── Internal library function ───────────────────────────────────

INTERNAL_LIB_VECTORS = [
    (0, 0),
    (1, 2),
    (21, 42),
    (2**127, 2**128),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x,expected", INTERNAL_LIB_VECTORS)
def test_internal_lib(
    advanced_client: au.AppClient, x: int, expected: int
) -> None:
    """MathLib.double(x) should return x+x."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="testInternalLib(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == expected


# ─── Inherited virtual/override + constructor args ───────────────

PROCESS_VECTORS = [
    (0, 0),       # 0 * 7 = 0
    (1, 7),       # 1 * 7 = 7
    (6, 42),      # 6 * 7 = 42
    (100, 700),   # 100 * 7 = 700
    (2**64, 7 * 2**64),
]


@pytest.mark.localnet
@pytest.mark.parametrize("x,expected", PROCESS_VECTORS)
def test_process_inherited(
    advanced_client: au.AppClient, x: int, expected: int
) -> None:
    """process(x) inherited from BaseProcessor should use multiplier=7."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="process(uint512)uint512",
            args=[x],
        )
    )
    assert result.abi_return == expected


# ─── High-level .staticcall stub ─────────────────────────────────

@pytest.mark.localnet
def test_staticcall_stub(advanced_client: au.AppClient) -> None:
    """address.staticcall should compile and return true (stubbed)."""
    result = advanced_client.send.call(
        au.AppClientMethodCallParams(
            method="testStaticCall()bool",
        )
    )
    assert result.abi_return is True
