"""
ECDSA / MessageHashUtils behavioral tests.
Tests EIP-191 and EIP-712 hash construction, and keccak256.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def ecdsa(localnet, account):
    return deploy_contract(localnet, account, "ECDSATest")


def keccak256(data: bytes) -> bytes:
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


def abi_bytes(ret) -> bytes:
    """Convert ABI return (list of ints for byte[N]) to bytes."""
    if isinstance(ret, (list, tuple)):
        return bytes(ret)
    return ret


def test_deploy(ecdsa):
    assert ecdsa.app_id > 0


def test_eth_signed_message_hash(ecdsa):
    """toEthSignedMessageHash should produce correct EIP-191 hash."""
    msg_hash = b"\x01" * 32
    result = ecdsa.send.call(
        au.AppClientMethodCallParams(
            method="testToEthSignedMessageHash",
            args=[msg_hash],
        )
    )
    # Expected: keccak256("\x19Ethereum Signed Message:\n32" + hash)
    prefix = b"\x19Ethereum Signed Message:\n32"
    expected = keccak256(prefix + msg_hash)
    assert abi_bytes(result.abi_return) == expected


def test_typed_data_hash(ecdsa):
    """toTypedDataHash should produce correct EIP-712 hash."""
    domain_sep = b"\xaa" * 32
    struct_hash = b"\xbb" * 32
    result = ecdsa.send.call(
        au.AppClientMethodCallParams(
            method="testToTypedDataHash",
            args=[domain_sep, struct_hash],
        )
    )
    # Expected: keccak256("\x19\x01" + domainSeparator + structHash)
    expected = keccak256(b"\x19\x01" + domain_sep + struct_hash)
    assert abi_bytes(result.abi_return) == expected


def test_keccak256_packed(ecdsa):
    """keccak256(abi.encodePacked(a, b)) should match Python reference."""
    a = b"\x11" * 32
    b = b"\x22" * 32
    result = ecdsa.send.call(
        au.AppClientMethodCallParams(
            method="testKeccak256Packed",
            args=[a, b],
        )
    )
    expected = keccak256(a + b)
    assert abi_bytes(result.abi_return) == expected


def test_eth_signed_message_hash_zero(ecdsa):
    """EIP-191 hash of zero hash."""
    msg_hash = b"\x00" * 32
    result = ecdsa.send.call(
        au.AppClientMethodCallParams(
            method="testToEthSignedMessageHash",
            args=[msg_hash],
        )
    )
    prefix = b"\x19Ethereum Signed Message:\n32"
    expected = keccak256(prefix + msg_hash)
    assert abi_bytes(result.abi_return) == expected
