"""Admin / role / pause tests for v2 contracts using Solady's OwnableRoles.

The previously-blocking `initialize(address)` arg-drop (the apparent
ABI-router missing-`dup` in `__postInit`'s first arg) was fixed in
`puya-sol: fix account ensureBiguint + __postInit ensure_budget hook`
[55fd3e233]. Root cause was actually upstream: `AssemblyBuilder::ensureBiguint`
silently coerced `account` values to `IntegerConstant(0)` in its non-scalar
fallback, so Solady's `or(newOwner, shl(255, iszero(newOwner)))` compiled to
a constant and the OWNER_SLOT got that constant rather than the caller's
address. puya was correctly identifying the parameter as dead because
puya-sol had already eliminated every reference to it. Fix routes `account`
through `ReinterpretCast(account → bytes → biguint)` (a runtime no-op).
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk import encoding
from conftest import AUTO_POPULATE, addr


def _call(client, method, args=None, sender=None, extra_fee=20_000, populate=AUTO_POPULATE):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=populate).abi_return


# ── Immutable getters (work fine — set by __postInit args) ────────────────

def test_onramp_collateral_token_getter(collateral_onramp, mock_token):
    """Constructor stored the COLLATERAL_TOKEN address from __postInit args."""
    expected_app_addr = encoding.encode_address(b"\x00" * 24 + mock_token.app_id.to_bytes(8, "big"))
    assert _call(collateral_onramp, "COLLATERAL_TOKEN") == expected_app_addr


def test_offramp_collateral_token_getter(collateral_offramp, mock_token):
    expected_app_addr = encoding.encode_address(b"\x00" * 24 + mock_token.app_id.to_bytes(8, "big"))
    assert _call(collateral_offramp, "COLLATERAL_TOKEN") == expected_app_addr


def test_permissioned_ramp_collateral_token_getter(permissioned_ramp, mock_token):
    expected_app_addr = encoding.encode_address(b"\x00" * 24 + mock_token.app_id.to_bytes(8, "big"))
    assert _call(permissioned_ramp, "COLLATERAL_TOKEN") == expected_app_addr


def test_ctf_adapter_constants(ctf_adapter, mock_token):
    """CtfCollateralAdapter exposes CONDITIONAL_TOKENS, COLLATERAL_TOKEN, USDCE."""
    expected = encoding.encode_address(b"\x00" * 24 + mock_token.app_id.to_bytes(8, "big"))
    assert _call(ctf_adapter, "CONDITIONAL_TOKENS") == expected
    assert _call(ctf_adapter, "COLLATERAL_TOKEN") == expected
    assert _call(ctf_adapter, "USDCE") == expected


# ── Pre-init state (works) ────────────────────────────────────────────────

def test_initial_paused_state_false(collateral_onramp, admin):
    assert _call(collateral_onramp, "paused", [addr(admin)]) is False


def test_permissioned_ramp_initial_nonce_zero(permissioned_ramp, funded_account):
    assert _call(permissioned_ramp, "nonces", [addr(funded_account)]) == 0


# ── ERC1155 receiver hook (pure view, works) ──────────────────────────────

def test_ctf_adapter_supports_ierc1155_receiver(ctf_adapter):
    """ERC165 supportsInterface returns true for IERC1155Receiver (0x4e2312e0)."""
    assert _call(ctf_adapter, "supportsInterface", [b"\x4e\x23\x12\xe0"]) is True


def test_ctf_adapter_does_not_support_random_interface(ctf_adapter):
    assert _call(ctf_adapter, "supportsInterface", [b"\xde\xad\xbe\xef"]) is False


# ── Owner-dependent operations ────────────────────────────────────────────

def test_initial_owner_is_admin(collateral_onramp, admin):
    assert _call(collateral_onramp, "owner") == admin.address


def test_transfer_ownership(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "transferOwnership", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "owner") == funded_account.address


def test_add_admin_grants_role(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "addAdmin", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "rolesOf", [addr(funded_account)]) != 0


def test_pause_unpause_flow(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "pause", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "paused", [addr(funded_account)]) is True
    _call(collateral_onramp, "unpause", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "paused", [addr(funded_account)]) is False


def test_permissioned_ramp_witness(permissioned_ramp, admin, funded_account):
    _call(permissioned_ramp, "addWitness", [addr(funded_account)], sender=admin)
    assert _call(permissioned_ramp, "rolesOf", [addr(funded_account)]) != 0


# ── Non-owner authorization (rightly reverts even with broken owner) ──────

def test_non_owner_cannot_addAdmin(collateral_onramp, funded_account):
    """Even with corrupted owner, a fresh account is also not the owner →
    addAdmin reverts. This test confirms the access-control gate fires."""
    with pytest.raises(LogicError):
        _call(collateral_onramp, "addAdmin", [addr(funded_account)],
              sender=funded_account)
