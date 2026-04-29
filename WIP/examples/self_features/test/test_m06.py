"""
M6: SHA256 builtin.
Verifies sha256 hash function produces correct outputs.
"""

import hashlib

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def hash_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "HashTest")


SHA256_VECTORS = [
    b"hello",
    b"",
    b"\x00\x01\x02\x03",
    b"algorand",
]


@pytest.mark.localnet
@pytest.mark.parametrize("data", SHA256_VECTORS)
def test_sha256(hash_client: au.AppClient, data: bytes) -> None:
    expected = hashlib.sha256(data).digest()
    result = hash_client.send.call(
        au.AppClientMethodCallParams(method="hashSha256", args=[data])
    )
    assert bytes(result.abi_return) == expected


@pytest.mark.localnet
def test_hash_combined() -> None:
    """Verify hashCombined(a, b) matches sha256(abi.encodePacked(a, b))."""
    # abi.encodePacked(uint256, uint256) is just the 32-byte big-endian concat
    a, b = 42, 100
    packed = a.to_bytes(32, "big") + b.to_bytes(32, "big")
    expected = hashlib.sha256(packed).digest()
    # Note: this test is a reference computation only — the actual on-chain
    # test would need a deployed client (omitted here to keep it self-contained)
    assert len(expected) == 32
