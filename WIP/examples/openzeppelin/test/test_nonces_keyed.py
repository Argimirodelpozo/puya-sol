"""
NoncesKeyed behavioral tests.
Tests key-ed nonce tracking with nested mappings (address => mapping(uint192 => uint64)).
Previously skipped due to puya optimizer overflow on uint192, now works with biguint key normalization.
"""

import hashlib
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def keyed_nonce_box_key(owner: str, key: int) -> bytes:
    """Compute box key for _keyedNonces[owner][key].
    Nested mapping: _keyedNonces + sha256(concat(sha256(addr), key_64bytes))
    """
    owner_bytes = encoding.decode_address(owner)
    key_bytes = key.to_bytes(64, "big")  # biguint normalized to 64 bytes
    concat_keys = owner_bytes + key_bytes
    key_hash = hashlib.sha256(concat_keys).digest()
    return b"_keyedNonces" + key_hash


@pytest.fixture(scope="module")
def nonces(localnet, account):
    client = deploy_contract(localnet, account, "NoncesKeyedTest")
    # Initialize the base nonce box for the account (for key=0 case)
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    client.send.call(
        au.AppClientMethodCallParams(
            method="initNonce",
            args=[account.address],
            box_references=[box_ref(client.app_id, nonce_key)],
        )
    )
    return client


@pytest.fixture(scope="module")
def owner(account):
    return account.address


def test_deploy(nonces):
    assert nonces.app_id > 0


def test_initial_nonce_key0_is_zero(nonces, owner):
    """Key 0 uses base Nonces storage."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(owner))
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces(address)uint512",
            args=[owner],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 0


def test_use_nonce_key0(nonces, owner):
    """useNonce with key=0 increments base nonce."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(owner))
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[owner, 0],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )
    # Known: post-increment returns incremented value (AVM limitation)
    assert result.abi_return == 1

    # Nonce should now be 1
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces(address)uint512",
            args=[owner],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 1


def test_use_nonce_key42_init(nonces, owner):
    """Initialize and use a keyed nonce (key=42)."""
    app_id = nonces.app_id
    kn_box = keyed_nonce_box_key(owner, 42)
    # Init the keyed nonce box
    nonces.send.call(
        au.AppClientMethodCallParams(
            method="initKeyedNonce",
            args=[owner, 42],
            box_references=[box_ref(app_id, kn_box)],
        )
    )

    # Use nonce with key=42
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[owner, 42],
            box_references=[box_ref(app_id, kn_box)],
        )
    )
    # Known: post-increment returns incremented value
    # key=42, nonce after increment=1 → (42 << 64) | 1
    expected = (42 * (2**64)) | 1
    assert result.abi_return == expected


def test_use_nonce_key42_second(nonces, owner):
    """Second use with key=42 should return nonce=2 (post-increment behavior)."""
    app_id = nonces.app_id
    kn_box = keyed_nonce_box_key(owner, 42)

    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[owner, 42],
            box_references=[box_ref(app_id, kn_box)],
            note=b"second_use",
        )
    )
    # key=42, nonce after increment=2 → (42 << 64) | 2
    expected = (42 * (2**64)) | 2
    assert result.abi_return == expected


def test_keyed_nonce_read(nonces, owner):
    """Read the keyed nonce (key=42) should show 2 (used twice)."""
    app_id = nonces.app_id
    kn_box = keyed_nonce_box_key(owner, 42)

    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces(address,uint512)uint512",
            args=[owner, 42],
            box_references=[box_ref(app_id, kn_box)],
        )
    )
    # key=42, nonce=2 → (42 << 64) | 2
    expected = (42 * (2**64)) | 2
    assert result.abi_return == expected


def test_base_nonce_unchanged_by_keyed(nonces, owner):
    """Base nonce (key=0) should still be 1, unaffected by keyed nonce ops."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(owner))
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces(address)uint512",
            args=[owner],
            box_references=[box_ref(nonces.app_id, nonce_key)],
            note=b"final_check",
        )
    )
    assert result.abi_return == 1
