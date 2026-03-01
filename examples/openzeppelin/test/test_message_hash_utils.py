"""
MessageHashUtils behavioral tests.
Tests EIP-191 and EIP-712 message hashing (pure functions).
"""

import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def hasher(localnet, account):
    return deploy_contract(localnet, account, "MessageHashUtilsTest")


def test_deploy(hasher):
    assert hasher.app_id > 0


def test_eth_signed_message_hash(hasher):
    """EIP-191 personal_sign: keccak256("\\x19Ethereum Signed Message:\\n32" || hash)."""
    # Use a known message hash (all zeros for simplicity)
    msg_hash = b"\x00" * 32
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="ethSignedMessageHash",
            args=[msg_hash],
        )
    )
    # Compute expected: keccak256(prefix + msg_hash)
    from Crypto.Hash import keccak
    prefix = b"\x19Ethereum Signed Message:\n32"
    k = keccak.new(digest_bits=256)
    k.update(prefix + msg_hash)
    expected = k.digest()
    assert bytes(result.abi_return) == expected


def test_eth_signed_message_hash_nonzero(hasher):
    """Test with a non-zero message hash."""
    msg_hash = bytes(range(32))
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="ethSignedMessageHash",
            args=[msg_hash],
        )
    )
    from Crypto.Hash import keccak
    prefix = b"\x19Ethereum Signed Message:\n32"
    k = keccak.new(digest_bits=256)
    k.update(prefix + msg_hash)
    expected = k.digest()
    assert bytes(result.abi_return) == expected


def test_typed_data_hash(hasher):
    """EIP-712: keccak256("\\x19\\x01" || domainSeparator || structHash)."""
    domain = b"\xaa" * 32
    struct = b"\xbb" * 32
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="typedDataHash",
            args=[domain, struct],
        )
    )
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(b"\x19\x01" + domain + struct)
    expected = k.digest()
    assert bytes(result.abi_return) == expected


def test_data_with_intended_validator_hash(hasher, account):
    """EIP-191 version 0x00: keccak256("\\x19\\x00" || validator || data)."""
    from algosdk import encoding
    validator = account.address
    data = b"hello world"
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="dataWithIntendedValidatorHash",
            args=[validator, data],
        )
    )
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(b"\x19\x00" + encoding.decode_address(validator) + data)
    expected = k.digest()
    assert bytes(result.abi_return) == expected


def test_typed_data_hash_zeros(hasher):
    """EIP-712 with zero hashes."""
    domain = b"\x00" * 32
    struct = b"\x00" * 32
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="typedDataHash",
            args=[domain, struct],
        )
    )
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(b"\x19\x01" + domain + struct)
    expected = k.digest()
    assert bytes(result.abi_return) == expected


def test_eth_signed_message_hash_bytes(hasher):
    """EIP-191 with arbitrary message bytes."""
    message = b"Hello, Algorand!"
    result = hasher.send.call(
        au.AppClientMethodCallParams(
            method="ethSignedMessageHashBytes",
            args=[message],
        )
    )
    from Crypto.Hash import keccak
    prefix = b"\x19Ethereum Signed Message:\n" + str(len(message)).encode()
    k = keccak.new(digest_bits=256)
    k.update(prefix + message)
    expected = k.digest()
    assert bytes(result.abi_return) == expected
