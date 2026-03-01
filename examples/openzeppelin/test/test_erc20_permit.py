"""
ERC20Permit behavioral tests.
Tests EIP-712 domain separator, permit hash construction, and nonce tracking.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def abi_bytes(ret) -> bytes:
    """Convert ABI return (list of ints for byte[N]) to bytes."""
    if isinstance(ret, (list, tuple)):
        return bytes(ret)
    return ret


def keccak256(data: bytes) -> bytes:
    """Python keccak256 for reference."""
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(data)
    return k.digest()


@pytest.fixture(scope="module")
def permit(localnet, account):
    return deploy_contract(localnet, account, "ERC20PermitTest")


def test_deploy(permit):
    assert permit.app_id > 0


def test_name(permit):
    result = permit.send.call(
        au.AppClientMethodCallParams(method="name")
    )
    assert result.abi_return == "PermitToken"


def test_symbol(permit):
    result = permit.send.call(
        au.AppClientMethodCallParams(method="symbol")
    )
    assert result.abi_return == "PMT"


def test_decimals(permit):
    result = permit.send.call(
        au.AppClientMethodCallParams(method="decimals")
    )
    assert result.abi_return == 18


def test_domain_separator(permit):
    """DOMAIN_SEPARATOR returns a deterministic non-zero bytes32."""
    result = permit.send.call(
        au.AppClientMethodCallParams(method="DOMAIN_SEPARATOR")
    )
    val = abi_bytes(result.abi_return)
    assert len(val) == 32
    assert val != b'\x00' * 32


def test_domain_separator_deterministic(permit):
    """DOMAIN_SEPARATOR returns the same value on repeated calls."""
    r1 = permit.send.call(
        au.AppClientMethodCallParams(method="DOMAIN_SEPARATOR")
    )
    r2 = permit.send.call(
        au.AppClientMethodCallParams(method="DOMAIN_SEPARATOR")
    )
    assert abi_bytes(r1.abi_return) == abi_bytes(r2.abi_return)


def test_domain_separator_matches_python(permit):
    """DOMAIN_SEPARATOR matches Python-computed reference value."""
    result = permit.send.call(
        au.AppClientMethodCallParams(method="DOMAIN_SEPARATOR")
    )
    # Compute expected: keccak256(TYPE_HASH + keccak256("PermitToken") + keccak256("1") + uint256(0) + uint256(0))
    type_hash = keccak256(b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
    name_hash = keccak256(b"PermitToken")
    version_hash = keccak256(b"1")
    chain_id = (0).to_bytes(32, "big")
    verifying_contract = (0).to_bytes(32, "big")
    expected = keccak256(type_hash + name_hash + version_hash + chain_id + verifying_contract)
    assert abi_bytes(result.abi_return) == expected


def test_get_permit_hash(permit, account):
    """getPermitHash returns a deterministic non-zero bytes32."""
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="getPermitHash",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    val = abi_bytes(result.abi_return)
    assert len(val) == 32
    assert val != b'\x00' * 32


def test_get_permit_hash_deterministic(permit, account):
    """Same inputs produce same permit hash."""
    r1 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getPermitHash",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    r2 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getPermitHash",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    assert abi_bytes(r1.abi_return) == abi_bytes(r2.abi_return)


def test_get_digest(permit, account):
    """getDigest returns a deterministic non-zero bytes32 (EIP-712 typed data hash)."""
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    val = abi_bytes(result.abi_return)
    assert len(val) == 32
    assert val != b'\x00' * 32


def test_digest_changes_with_value(permit, account):
    """Different values produce different digests."""
    r1 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    r2 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 2000, 0, 999999],
        )
    )
    assert abi_bytes(r1.abi_return) != abi_bytes(r2.abi_return)


def test_digest_changes_with_nonce(permit, account):
    """Different nonces produce different digests."""
    r1 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    r2 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 1, 999999],
        )
    )
    assert abi_bytes(r1.abi_return) != abi_bytes(r2.abi_return)


def test_digest_changes_with_deadline(permit, account):
    """Different deadlines produce different digests."""
    r1 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 0, 999999],
        )
    )
    r2 = permit.send.call(
        au.AppClientMethodCallParams(
            method="getDigest",
            args=[account.address, account.address, 1000, 0, 888888],
        )
    )
    assert abi_bytes(r1.abi_return) != abi_bytes(r2.abi_return)


def test_nonce_initial_zero(permit, account):
    """Initial nonce should be 0 (default for uninitialized mapping)."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="nonces",
            args=[account.address],
            box_references=[box_ref(permit.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 0


def test_use_nonce_increments(permit, account):
    """useNonce increments the nonce and returns the old value."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    # First useNonce: should return 0 and set to 1
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[account.address],
            box_references=[box_ref(permit.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 0

    # Read nonce: should now be 1
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="nonces",
            args=[account.address],
            box_references=[box_ref(permit.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 1


def test_use_nonce_twice(permit, account):
    """Second useNonce should return 1 and set to 2."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[account.address],
            box_references=[box_ref(permit.app_id, nonce_key)],
            note=b"second_use",
        )
    )
    assert result.abi_return == 1

    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="nonces",
            args=[account.address],
            box_references=[box_ref(permit.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 2


def test_mint_and_balance(permit, account):
    """Mint tokens and check balance."""
    bal_key = mapping_box_key("_balances", addr_bytes(account.address))
    permit.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[account.address, 1000000],
            box_references=[box_ref(permit.app_id, bal_key)],
        )
    )
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box_ref(permit.app_id, bal_key)],
        )
    )
    assert result.abi_return == 1000000


def test_total_supply_after_mint(permit):
    """totalSupply should reflect minted amount."""
    result = permit.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    assert result.abi_return == 1000000


def test_approve_and_allowance(permit, account):
    """approve sets allowance correctly."""
    allowance_key = mapping_box_key(
        "_allowances", addr_bytes(account.address), addr_bytes(account.address)
    )
    permit.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 500],
            box_references=[box_ref(permit.app_id, allowance_key)],
        )
    )
    result = permit.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[account.address, account.address],
            box_references=[box_ref(permit.app_id, allowance_key)],
        )
    )
    assert result.abi_return == 500
