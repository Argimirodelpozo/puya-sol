"""
AAVE V4 NoncesKeyed tests.
"""

import pytest
import hashlib
import algokit_utils as au
from conftest import deploy_contract


def _box_ref(app_id, key):
    return au.BoxReference(app_id=app_id, name=key)


def _mapping_box_key(mapping_name, key_bytes):
    return mapping_name.encode() + hashlib.sha256(key_bytes).digest()


def _composite_key(*parts):
    """Concatenate multiple key parts for multi-key mapping lookup."""
    return b''.join(parts)


def _account_key(address_str):
    """Convert Algorand address to 32-byte key."""
    from algosdk import encoding
    return encoding.decode_address(address_str)


def _biguint_key(val):
    """Normalize biguint to 64-byte key."""
    raw = val.to_bytes((val.bit_length() + 7) // 8, 'big') if val > 0 else b'\x00'
    padded = b'\x00' * 64 + raw
    return padded[len(padded) - 64:]


@pytest.fixture(scope="module")
def nonces(localnet, account):
    return deploy_contract(localnet, account, "NoncesKeyed")


_call_counter = 0

def _call(client, method, *args, boxes=None):
    global _call_counter
    _call_counter += 1
    note = f"call_{_call_counter}".encode()
    if boxes:
        params = au.AppClientMethodCallParams(
            method=method, args=list(args), box_references=boxes, note=note,
        )
    else:
        params = au.AppClientMethodCallParams(method=method, args=list(args), note=note)
    result = client.send.call(params)
    return result.abi_return


def test_deploy(nonces):
    assert nonces.app_id > 0


def test_nonces_initial_zero(nonces, account):
    """Initial nonce for any (owner, key) should be 0."""
    from algosdk import encoding
    owner = encoding.decode_address(account.address)
    key = 0
    # Box key for _nonces mapping: _nonces + sha256(owner_32 + biguint_key(0))
    composite = _composite_key(_account_key(account.address), _biguint_key(key))
    box_key = _mapping_box_key("_nonces", composite)
    box = _box_ref(nonces.app_id, box_key)
    result = _call(nonces, "nonces", account.address, key, boxes=[box])
    assert result == 0


def test_useNonce_increments(nonces, account):
    """useNonce should return packed(key, nonce) and increment."""
    key = 42
    composite = _composite_key(_account_key(account.address), _biguint_key(key))
    box_key = _mapping_box_key("_nonces", composite)
    box = _box_ref(nonces.app_id, box_key)
    # First use: returns packed(key=42, nonce_BEFORE_increment) — the
    # standard EIP-2612 useNonce semantics: read the current nonce
    # (= 0 on first use), then increment. So the call returns
    # `(key << 64) | 0`, and the stored nonce afterwards is 1.
    # (An earlier puya-sol revision inverted this to post-increment;
    # the current version is correct.)
    result = _call(nonces, "useNonce", key, boxes=[box])
    packed_key = key * (2**64)
    assert result == packed_key + 0
    result2 = _call(nonces, "nonces", account.address, key, boxes=[box])
    assert result2 == packed_key + 1


def test_useNonce_different_keys(nonces, account):
    """Different keys should have independent nonces."""
    key1 = 100
    key2 = 200
    composite1 = _composite_key(_account_key(account.address), _biguint_key(key1))
    composite2 = _composite_key(_account_key(account.address), _biguint_key(key2))
    box1 = _box_ref(nonces.app_id, _mapping_box_key("_nonces", composite1))
    box2 = _box_ref(nonces.app_id, _mapping_box_key("_nonces", composite2))

    _call(nonces, "useNonce", key1, boxes=[box1])
    _call(nonces, "useNonce", key1, boxes=[box1])
    packed1 = key1 * (2**64)
    packed2 = key2 * (2**64)
    # key1 used twice → nonce=2, key2 unused → nonce=0
    assert _call(nonces, "nonces", account.address, key1, boxes=[box1]) == packed1 + 2
    assert _call(nonces, "nonces", account.address, key2, boxes=[box2]) == packed2 + 0


# ─── useCheckedNonce ──────────────────────────────────────────────────────────
# Ported from upstream tests/contracts/utils/NoncesKeyed.t.sol — needs
# NoncesKeyedMock since `_useCheckedNonce` is internal in the base
# contract (matches upstream's setup).


@pytest.fixture(scope="module")
def mock(localnet, account):
    return deploy_contract(localnet, account, "NoncesKeyedMock")


def test_useCheckedNonce_monotonic(mock, account):
    """Calling useCheckedNonce with the CURRENT packed nonce succeeds
    and increments. Direct port of upstream test_useCheckedNonce_monotonic
    (without vm.setArbitraryStorage — we use a fresh app, key=7)."""
    from algosdk import encoding
    key = 7
    composite = _composite_key(_account_key(account.address), _biguint_key(key))
    box_key = _mapping_box_key("_nonces", composite)
    box = _box_ref(mock.app_id, box_key)

    # Initial keyNonce = (key << 64) | 0
    current_keyNonce = _call(mock, "nonces", account.address, key, boxes=[box])
    packed_key = key * (2 ** 64)
    assert current_keyNonce == packed_key + 0

    _call(mock, "useCheckedNonce", account.address, current_keyNonce, boxes=[box])

    # After: nonce incremented → packed_key + 1
    next_keyNonce = _call(mock, "nonces", account.address, key, boxes=[box])
    assert next_keyNonce == packed_key + 1


def test_useCheckedNonce_revertsWith_InvalidAccountNonce(mock, account):
    """Calling useCheckedNonce with the WRONG packed nonce reverts
    with InvalidAccountNonce(owner, currentNonce). Direct port of
    the upstream revert test."""
    from algokit_utils.errors.logic_error import LogicError
    key = 9
    composite = _composite_key(_account_key(account.address), _biguint_key(key))
    box_key = _mapping_box_key("_nonces", composite)
    box = _box_ref(mock.app_id, box_key)

    # Burn one nonce so the current is 1, not 0.
    _call(mock, "useNonce", key, boxes=[box])
    packed_key = key * (2 ** 64)
    current = _call(mock, "nonces", account.address, key, boxes=[box])
    assert current == packed_key + 1

    # Off-by-one: the call expects current (packed_key + 1), but pass
    # an arbitrary stale or future value → revert.
    invalid_keyNonce = packed_key + 99
    with pytest.raises(LogicError):
        _call(mock, "useCheckedNonce", account.address, invalid_keyNonce, boxes=[box])
