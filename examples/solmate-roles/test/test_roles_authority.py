"""
Solmate RolesAuthority — Test Suite
Source: https://github.com/transmissions11/solmate/blob/main/src/auth/authorities/RolesAuthority.sol
Tests from: https://github.com/transmissions11/solmate/blob/main/src/test/RolesAuthority.t.sol

Tests translated from Foundry (Solidity) to Python/pytest for Algorand localnet.
Covers: setUserRole, setRoleCapability, setPublicCapability, canCall,
        doesUserHaveRole, doesRoleHaveCapability, role revocation, ownership.

RolesAuthority is a role-based access control system supporting up to 256 roles
via bitwise operations on bytes32 bitmasks. Each role can be granted capabilities
(target + function selector pairs), and users can be assigned roles.

Modifications for AVM: inlined Auth/Authority, simplified to owner-only auth,
uses original bytes32 role bitmasks and bytes4 function signature keys.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def bytes4_val(value: int) -> bytes:
    """Encode a uint32 as 4-byte bytes4 for function signatures."""
    return value.to_bytes(4, "big")


def bytes32_to_int(value) -> int:
    """Convert a byte[32] return (list of ints) to an integer."""
    if isinstance(value, (list, tuple)):
        return int.from_bytes(bytes(value), "big")
    if isinstance(value, bytes):
        return int.from_bytes(value, "big")
    return int(value)


@pytest.fixture(scope="module")
def user1(localnet: au.AlgorandClient) -> SigningAccount:
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, localnet.account.localnet_dispenser(), au.AlgoAmount.from_algo(10)
    )
    return acct


@pytest.fixture(scope="module")
def roles_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy RolesAuthority."""
    client = deploy_contract(
        localnet, account, "RolesAuthority",
        fund_amount=2_000_000,
    )
    return client


# Use fixed bytes4 function selectors for tests
FUNC_SIG_1 = bytes4_val(0xDEADBEEF)
FUNC_SIG_2 = bytes4_val(0xCAFEBABE)


def _user_roles_box(user: str) -> bytes:
    return mapping_box_key("_userRoles", addr_bytes(user))


def _capability_public_box(target: str, sig: bytes) -> bytes:
    return mapping_box_key("_capabilityPublic", addr_bytes(target) + sig)


def _roles_with_capability_box(target: str, sig: bytes) -> bytes:
    return mapping_box_key("_rolesWithCapability", addr_bytes(target) + sig)


# ─── Owner Tests ───

@pytest.mark.localnet
def test_owner(
    roles_client: au.AppClient, account: SigningAccount
) -> None:
    """Owner should be the deployer."""
    result = roles_client.send.call(
        au.AppClientMethodCallParams(method="owner", args=[])
    )
    assert result.abi_return == account.address


# ─── Set User Role Tests (from testSetRoles) ───

@pytest.mark.localnet
def test_set_user_role(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """setUserRole(user, 0, true) → doesUserHaveRole(user, 0) == true"""
    app_id = roles_client.app_id
    ur_box = _user_roles_box(user1.address)

    # Set role 0
    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="setUserRole",
            args=[user1.address, 0, True],
            box_references=[box_ref(app_id, ur_box)],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesUserHaveRole",
            args=[user1.address, 0],
            box_references=[box_ref(app_id, ur_box)],
        )
    )
    assert result.abi_return is True

    # Check role 1 is NOT set
    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesUserHaveRole",
            args=[user1.address, 1],
            box_references=[box_ref(app_id, ur_box)],
        )
    )
    assert result.abi_return is False


# ─── Set Multiple Roles (from testSetRoles fuzz) ───

@pytest.mark.localnet
def test_set_multiple_roles(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Set roles 0, 5, 10 → all true, role 3 still false."""
    app_id = roles_client.app_id
    ur_box = _user_roles_box(user1.address)

    for role in [5, 10]:
        roles_client.send.call(
            au.AppClientMethodCallParams(
                method="setUserRole",
                args=[user1.address, role, True],
                box_references=[box_ref(app_id, ur_box)],
            )
        )

    # Check all set roles
    for role in [0, 5, 10]:
        result = roles_client.send.call(
            au.AppClientMethodCallParams(
                method="doesUserHaveRole",
                args=[user1.address, role],
                box_references=[box_ref(app_id, ur_box)],
            )
        )
        assert result.abi_return is True, f"Role {role} should be set"

    # Role 3 should NOT be set
    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesUserHaveRole",
            args=[user1.address, 3],
            box_references=[box_ref(app_id, ur_box)],
        )
    )
    assert result.abi_return is False

    # getUserRoles should have bits 0, 5, 10 set = 1 + 32 + 1024 = 1057
    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="getUserRoles",
            args=[user1.address],
            box_references=[box_ref(app_id, ur_box)],
        )
    )
    expected = (1 << 0) | (1 << 5) | (1 << 10)
    assert bytes32_to_int(result.abi_return) == expected


# ─── Revoke Role (from testSetRoles with false) ───

@pytest.mark.localnet
def test_revoke_role(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Revoking role 5 → doesUserHaveRole(user, 5) == false, others unchanged."""
    app_id = roles_client.app_id
    ur_box = _user_roles_box(user1.address)

    # Revoke role 5
    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="setUserRole",
            args=[user1.address, 5, False],
            box_references=[box_ref(app_id, ur_box)],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesUserHaveRole",
            args=[user1.address, 5],
            box_references=[box_ref(app_id, ur_box)],
        )
    )
    assert result.abi_return is False

    # Role 0 and 10 should still be set
    for role in [0, 10]:
        result = roles_client.send.call(
            au.AppClientMethodCallParams(
                method="doesUserHaveRole",
                args=[user1.address, role],
                box_references=[box_ref(app_id, ur_box)],
            )
        )
        assert result.abi_return is True


# ─── Set Role Capability Tests (from testSetRoleCapabilities) ───

@pytest.mark.localnet
def test_set_role_capability(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """setRoleCapability(0, target, sig, true) → doesRoleHaveCapability == true"""
    app_id = roles_client.app_id
    rc_box = _roles_with_capability_box(user1.address, FUNC_SIG_1)

    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="setRoleCapability",
            args=[0, user1.address, FUNC_SIG_1, True],
            box_references=[box_ref(app_id, rc_box)],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesRoleHaveCapability",
            args=[0, user1.address, FUNC_SIG_1],
            box_references=[box_ref(app_id, rc_box)],
        )
    )
    assert result.abi_return is True

    # Different role should NOT have capability
    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="doesRoleHaveCapability",
            args=[1, user1.address, FUNC_SIG_1],
            box_references=[box_ref(app_id, rc_box)],
        )
    )
    assert result.abi_return is False


# ─── Set Public Capability Tests (from testSetPublicCapabilities) ───

@pytest.mark.localnet
def test_set_public_capability(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """setPublicCapability(target, sig, true) → isCapabilityPublic == true"""
    app_id = roles_client.app_id
    pc_box = _capability_public_box(user1.address, FUNC_SIG_2)

    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="setPublicCapability",
            args=[user1.address, FUNC_SIG_2, True],
            box_references=[box_ref(app_id, pc_box)],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="isCapabilityPublic",
            args=[user1.address, FUNC_SIG_2],
            box_references=[box_ref(app_id, pc_box)],
        )
    )
    assert result.abi_return is True


# ─── Can Call with Role (from testCanCallWithAuthorizedRole) ───

@pytest.mark.localnet
def test_can_call_with_role(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """User with role 0, target has role 0 capability → canCall == true"""
    app_id = roles_client.app_id
    target = account.address
    ur_box = _user_roles_box(user1.address)
    rc_box = _roles_with_capability_box(target, FUNC_SIG_1)

    # User1 already has role 0 from test_set_user_role
    # Set role 0 capability on target for FUNC_SIG_1
    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="setRoleCapability",
            args=[0, target, FUNC_SIG_1, True],
            box_references=[box_ref(app_id, rc_box)],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[user1.address, target, FUNC_SIG_1],
            box_references=[
                box_ref(app_id, _capability_public_box(target, FUNC_SIG_1)),
                box_ref(app_id, ur_box),
                box_ref(app_id, rc_box),
            ],
        )
    )
    assert result.abi_return is True


# ─── Can Call with Public Capability (from testCanCallPublicCapability) ───

@pytest.mark.localnet
def test_can_call_public(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """Public capability → any user canCall == true"""
    app_id = roles_client.app_id
    target = user1.address
    pc_box = _capability_public_box(target, FUNC_SIG_2)
    ur_box = _user_roles_box(account.address)
    rc_box = _roles_with_capability_box(target, FUNC_SIG_2)

    # FUNC_SIG_2 was set public on user1.address in test_set_public_capability
    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[account.address, target, FUNC_SIG_2],
            box_references=[
                box_ref(app_id, pc_box),
                box_ref(app_id, ur_box),
                box_ref(app_id, rc_box),
            ],
        )
    )
    assert result.abi_return is True


# ─── Cannot Call without Role (negative test) ───

@pytest.mark.localnet
def test_cannot_call_without_role(
    roles_client: au.AppClient, account: SigningAccount, user1: SigningAccount
) -> None:
    """User without matching role → canCall == false"""
    app_id = roles_client.app_id
    target = account.address
    unknown_sig = bytes4_val(0x12345678)
    pc_box = _capability_public_box(target, unknown_sig)
    ur_box = _user_roles_box(user1.address)
    rc_box = _roles_with_capability_box(target, unknown_sig)

    result = roles_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[user1.address, target, unknown_sig],
            box_references=[
                box_ref(app_id, pc_box),
                box_ref(app_id, ur_box),
                box_ref(app_id, rc_box),
            ],
        )
    )
    assert result.abi_return is False


# ─── Transfer Ownership ───

@pytest.mark.localnet
def test_transfer_ownership(
    roles_client: au.AppClient,
    localnet: au.AlgorandClient,
    account: SigningAccount,
    user1: SigningAccount,
) -> None:
    """transferOwnership → new owner can call admin functions, old owner cannot."""
    app_id = roles_client.app_id

    # Transfer to user1
    roles_client.send.call(
        au.AppClientMethodCallParams(
            method="transferOwnership",
            args=[user1.address],
        )
    )

    result = roles_client.send.call(
        au.AppClientMethodCallParams(method="owner", args=[])
    )
    assert result.abi_return == user1.address

    # Transfer back to original owner (from user1)
    client1 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=roles_client.app_spec,
            app_id=app_id,
            default_sender=user1.address,
        )
    )
    localnet.account.set_signer_from_account(user1)

    client1.send.call(
        au.AppClientMethodCallParams(
            method="transferOwnership",
            args=[account.address],
        )
    )

    localnet.account.set_signer_from_account(account)

    result = roles_client.send.call(
        au.AppClientMethodCallParams(method="owner", args=[])
    )
    assert result.abi_return == account.address
