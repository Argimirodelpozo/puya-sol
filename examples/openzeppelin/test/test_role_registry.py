"""
RoleRegistry behavioral tests.
Tests role-based access with expiration, bytes32 keys, keccak hashing.
"""

import hashlib
import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding
from Crypto.Hash import keccak


@pytest.fixture(scope="module")
def registry(localnet, account):
    return deploy_contract(localnet, account, "RoleRegistry")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def keccak_str(s: str) -> bytes:
    """Compute keccak256 of a string (like Solidity keccak256("ADMIN_ROLE"))."""
    h = keccak.new(digest_bits=256)
    h.update(s.encode())
    return h.digest()


def role_key_box(role_hash: bytes, account_addr: str) -> bytes:
    """Box key for _roles mapping: keccak256(role, account)."""
    addr = encoding.decode_address(account_addr)
    h = keccak.new(digest_bits=256)
    h.update(role_hash + addr)
    key = h.digest()
    return mapping_box_key("_roles", key)


def expiry_key_box(role_hash: bytes, account_addr: str) -> bytes:
    """Box key for _roleExpiry mapping."""
    addr = encoding.decode_address(account_addr)
    h = keccak.new(digest_bits=256)
    h.update(role_hash + addr)
    key = h.digest()
    return mapping_box_key("_roleExpiry", key)


ADMIN_ROLE = keccak_str("ADMIN_ROLE")
OPERATOR_ROLE = keccak_str("OPERATOR_ROLE")
MINTER_ROLE = keccak_str("MINTER_ROLE")


# --- Deploy ---

def test_deploy(registry):
    assert registry.app_id > 0


def test_admin(registry, account):
    result = registry.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert result.abi_return == account.address


def test_is_admin(registry, account):
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="isAdmin",
            args=[addr_bytes(account.address)],
        )
    )
    assert result.abi_return is True


# --- Grant role ---

def test_grant_operator_role(registry, account, localnet):
    app_id = registry.app_id
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)

    rk = role_key_box(OPERATOR_ROLE, account2.address)
    ek = expiry_key_box(OPERATOR_ROLE, account2.address)
    count_key = mapping_box_key("_roleMemberCount", OPERATOR_ROLE)

    registry.send.call(
        au.AppClientMethodCallParams(
            method="grantRole",
            args=[OPERATOR_ROLE, addr_bytes(account2.address), 0],  # no expiry
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
                box_ref(app_id, count_key),
            ],
        )
    )

    # Check role
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[OPERATOR_ROLE, addr_bytes(account2.address)],
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
            ],
        )
    )
    assert result.abi_return is True


def test_role_member_count(registry):
    app_id = registry.app_id
    count_key = mapping_box_key("_roleMemberCount", OPERATOR_ROLE)
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="getRoleMemberCount",
            args=[OPERATOR_ROLE],
            box_references=[box_ref(app_id, count_key)],
        )
    )
    assert result.abi_return == 1


# --- Grant minter role ---

def test_grant_minter_role(registry, account):
    app_id = registry.app_id
    rk = role_key_box(MINTER_ROLE, account.address)
    ek = expiry_key_box(MINTER_ROLE, account.address)
    count_key = mapping_box_key("_roleMemberCount", MINTER_ROLE)

    registry.send.call(
        au.AppClientMethodCallParams(
            method="grantRole",
            args=[MINTER_ROLE, addr_bytes(account.address), 0],
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
                box_ref(app_id, count_key),
            ],
        )
    )


def test_has_minter_role(registry, account):
    app_id = registry.app_id
    rk = role_key_box(MINTER_ROLE, account.address)
    ek = expiry_key_box(MINTER_ROLE, account.address)
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[MINTER_ROLE, addr_bytes(account.address)],
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
            ],
        )
    )
    assert result.abi_return is True


# --- Cannot grant duplicate ---

def test_cannot_grant_duplicate(registry, account):
    app_id = registry.app_id
    rk = role_key_box(MINTER_ROLE, account.address)
    ek = expiry_key_box(MINTER_ROLE, account.address)
    count_key = mapping_box_key("_roleMemberCount", MINTER_ROLE)

    with pytest.raises(Exception):
        registry.send.call(
            au.AppClientMethodCallParams(
                method="grantRole",
                args=[MINTER_ROLE, addr_bytes(account.address), 0],
                box_references=[
                    box_ref(app_id, rk),
                    box_ref(app_id, ek),
                    box_ref(app_id, count_key),
                ],
            )
        )


# --- Revoke role ---

def test_revoke_minter_role(registry, account):
    app_id = registry.app_id
    rk = role_key_box(MINTER_ROLE, account.address)
    ek = expiry_key_box(MINTER_ROLE, account.address)
    count_key = mapping_box_key("_roleMemberCount", MINTER_ROLE)

    registry.send.call(
        au.AppClientMethodCallParams(
            method="revokeRole",
            args=[MINTER_ROLE, addr_bytes(account.address)],
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
                box_ref(app_id, count_key),
            ],
        )
    )

    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="hasRole",
            args=[MINTER_ROLE, addr_bytes(account.address)],
            box_references=[
                box_ref(app_id, rk),
                box_ref(app_id, ek),
            ],
        )
    )
    assert result.abi_return is False


# --- Non-admin cannot grant ---

def test_non_admin_cannot_grant(registry, account, localnet):
    app_id = registry.app_id
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)

    rk = role_key_box(ADMIN_ROLE, account2.address)
    ek = expiry_key_box(ADMIN_ROLE, account2.address)
    count_key = mapping_box_key("_roleMemberCount", ADMIN_ROLE)

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=registry.app_spec,
            app_id=registry.app_id,
            default_sender=account2.address,
        )
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="grantRole",
                args=[ADMIN_ROLE, addr_bytes(account2.address), 0],
                box_references=[
                    box_ref(app_id, rk),
                    box_ref(app_id, ek),
                    box_ref(app_id, count_key),
                ],
            )
        )


# --- Transfer admin ---

def test_transfer_admin(registry, account, localnet):
    app_id = registry.app_id
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)

    registry.send.call(
        au.AppClientMethodCallParams(
            method="transferAdmin",
            args=[addr_bytes(account2.address)],
        )
    )

    result = registry.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert result.abi_return == account2.address

    # Transfer back
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=registry.app_spec,
            app_id=registry.app_id,
            default_sender=account2.address,
        )
    )
    client2.send.call(
        au.AppClientMethodCallParams(
            method="transferAdmin",
            args=[addr_bytes(account.address)],
        )
    )

    result = registry.send.call(au.AppClientMethodCallParams(method="getAdmin"))
    assert result.abi_return == account.address
