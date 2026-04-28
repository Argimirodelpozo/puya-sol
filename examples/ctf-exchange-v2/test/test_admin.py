"""Admin / role / pause tests for v2 contracts using Solady's OwnableRoles.

KNOWN ISSUE: Solady's `_initializeOwner` and `_setOwner` use the EVM-specific
address-cleanup pattern `shr(96, shl(96, newOwner))` to mask upper 96 bits of
a 256-bit word holding a 20-byte address. On AVM with 32-byte addresses this
silently truncates the owner address, leaving 12 leading bytes zero. As a
result, `owner()` returns a corrupted value and every `onlyOwner` check
reverts when called from a non-app account.

This affects every Solady-Ownable-derived contract until either puya-sol
recognizes the shl/shr address-cleanup pattern as a no-op for 32-byte
addresses, or the Solady source is patched. Tests that depend on a working
owner are marked xfail with the reason; getter tests that don't go through
ownership work fine.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk import encoding
from conftest import AUTO_POPULATE, addr


SOLADY_OWNERSHIP_BROKEN = "Solady's shl/shr address-cleanup truncates 32-byte AVM addresses"


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


# ── Owner-dependent operations (xfail until address-cleanup fix) ──────────

@pytest.mark.xfail(reason=SOLADY_OWNERSHIP_BROKEN)
def test_initial_owner_is_admin(collateral_onramp, admin):
    assert _call(collateral_onramp, "owner") == admin.address


@pytest.mark.xfail(reason=SOLADY_OWNERSHIP_BROKEN)
def test_transfer_ownership(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "transferOwnership", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "owner") == funded_account.address


@pytest.mark.xfail(reason=SOLADY_OWNERSHIP_BROKEN)
def test_add_admin_grants_role(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "addAdmin", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "rolesOf", [addr(funded_account)]) != 0


@pytest.mark.xfail(reason=SOLADY_OWNERSHIP_BROKEN)
def test_pause_unpause_flow(collateral_onramp, admin, funded_account):
    _call(collateral_onramp, "pause", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "paused", [addr(funded_account)]) is True
    _call(collateral_onramp, "unpause", [addr(funded_account)], sender=admin)
    assert _call(collateral_onramp, "paused", [addr(funded_account)]) is False


@pytest.mark.xfail(reason=SOLADY_OWNERSHIP_BROKEN)
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
