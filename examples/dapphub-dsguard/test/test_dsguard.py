"""
DappHub DSGuard — Test Suite
Source: https://github.com/dapphub/ds-guard/blob/master/src/guard.sol
Tests from: https://github.com/dapphub/ds-guard/blob/master/src/guard.t.sol

Tests translated from DappTools (Solidity) to Python/pytest for Algorand localnet.
Covers: permit, forbid, canCall with wildcard expansion, getAcl direct lookup,
        ALL 8 wildcard combinations, ownership.

DSGuard is a 3D access control list (src, dst, sig) → bool.
The ANY constant (bytes32 all 1s) acts as a wildcard that matches everything.
canCall checks all 8 combinations: (specific/ANY)^3.

Modifications for AVM: inlined DSAuth (simplified to owner-only), uses bytes32
for all ACL dimensions. Added explicit getters.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract, mapping_box_key, box_ref


def b32(value: int) -> bytes:
    """Encode an integer as 32-byte bytes32 for ABI args and mapping keys."""
    return value.to_bytes(32, "big")


@pytest.fixture(scope="module")
def guard_client(
    localnet: au.AlgorandClient,
    account: SigningAccount,
) -> au.AppClient:
    """Deploy DSGuard."""
    client = deploy_contract(
        localnet, account, "DSGuard",
        fund_amount=2_000_000,
    )
    return client


# Constants for test identifiers (representing src, dst, sig)
SRC_ALICE = 1
SRC_BOB = 2
DST_VAULT = 10
DST_TREASURY = 20
SIG_TRANSFER = 0xA9059CBB  # transfer(address,uint256)
SIG_APPROVE = 0x095EA7B3   # approve(address,uint256)
ANY = 2**256 - 1  # type(uint256).max — bytes32 all 0xFF


def _acl_box(src: int, dst: int, sig: int) -> bytes:
    """Box key for _acl[src][dst][sig]. Keys are bytes32 (32 bytes each)."""
    return mapping_box_key("_acl", b32(src) + b32(dst) + b32(sig))


# ─── Owner Tests ───

@pytest.mark.localnet
def test_owner(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """Owner should be the deployer."""
    result = guard_client.send.call(
        au.AppClientMethodCallParams(method="owner", args=[])
    )
    assert result.abi_return == account.address


# ─── Permit Tests ───

@pytest.mark.localnet
def test_permit(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """permit(src, dst, sig) → getAcl(src, dst, sig) == true"""
    app_id = guard_client.app_id
    acl_box = _acl_box(SRC_ALICE, DST_VAULT, SIG_TRANSFER)

    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(SRC_ALICE), b32(DST_VAULT), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_ALICE), b32(DST_VAULT), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )
    assert result.abi_return is True


# ─── Forbid Tests ───

@pytest.mark.localnet
def test_forbid(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """permit then forbid → getAcl returns false"""
    app_id = guard_client.app_id
    acl_box = _acl_box(SRC_BOB, DST_VAULT, SIG_APPROVE)

    # Permit first
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(SRC_BOB), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )
    assert result.abi_return is True

    # Forbid
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="forbid",
            args=[b32(SRC_BOB), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )
    assert result.abi_return is False


# ─── canCall with Specific Entry ───

@pytest.mark.localnet
def test_can_call_specific(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """canCall returns true for a specific (src, dst, sig) permit."""
    app_id = guard_client.app_id

    # SRC_ALICE → DST_VAULT → SIG_TRANSFER was permitted in test_permit
    # canCall checks 8 boxes but the specific one should hit
    boxes = [
        box_ref(app_id, _acl_box(SRC_ALICE, DST_VAULT, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(SRC_ALICE, DST_VAULT, ANY)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, DST_VAULT, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(ANY, DST_VAULT, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(SRC_ALICE), b32(DST_VAULT), b32(SIG_TRANSFER)],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


# ─── canCall returns false for unpermitted entry ───

@pytest.mark.localnet
def test_can_call_returns_false(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """canCall returns false when no entry matches."""
    app_id = guard_client.app_id

    boxes = [
        box_ref(app_id, _acl_box(SRC_BOB, DST_TREASURY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(SRC_BOB, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(SRC_BOB, ANY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(SRC_BOB, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, SIG_TRANSFER)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_TRANSFER)],
            box_references=boxes,
        )
    )
    assert result.abi_return is False


# ─── Wildcard ANY on sig dimension ───

@pytest.mark.localnet
def test_wildcard_sig(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """permit(src, dst, ANY) → canCall(src, dst, any_sig) == true"""
    app_id = guard_client.app_id

    # Permit Alice → Treasury for ANY function
    acl_box = _acl_box(SRC_ALICE, DST_TREASURY, ANY)
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(SRC_ALICE), b32(DST_TREASURY), b32(ANY)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    # canCall with specific sig should match via wildcard
    boxes = [
        box_ref(app_id, _acl_box(SRC_ALICE, DST_TREASURY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_ALICE, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(SRC_ALICE), b32(DST_TREASURY), b32(SIG_APPROVE)],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


# ─── Wildcard ANY on src dimension ───

@pytest.mark.localnet
def test_wildcard_src(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """permit(ANY, dst, sig) → canCall(any_src, dst, sig) == true"""
    app_id = guard_client.app_id

    # Permit ANY source → Vault for approve
    acl_box = _acl_box(ANY, DST_VAULT, SIG_APPROVE)
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(ANY), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    # Bob can call Vault.approve via wildcard src
    boxes = [
        box_ref(app_id, _acl_box(SRC_BOB, DST_VAULT, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_BOB, DST_VAULT, ANY)),
        box_ref(app_id, _acl_box(SRC_BOB, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_BOB, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, DST_VAULT, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, DST_VAULT, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(SRC_BOB), b32(DST_VAULT), b32(SIG_APPROVE)],
            box_references=boxes,
        )
    )
    assert result.abi_return is True


# ─── Full Wildcard ANY ───

@pytest.mark.localnet
def test_wildcard_all(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """permit(ANY, ANY, ANY) → canCall(anything, anything, anything) == true"""
    app_id = guard_client.app_id

    # Permit everything
    acl_box = _acl_box(ANY, ANY, ANY)
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(ANY), b32(ANY), b32(ANY)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    # Any random values should match
    boxes = [
        box_ref(app_id, _acl_box(999, 888, 777)),
        box_ref(app_id, _acl_box(999, 888, ANY)),
        box_ref(app_id, _acl_box(999, ANY, 777)),
        box_ref(app_id, _acl_box(999, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, 888, 777)),
        box_ref(app_id, _acl_box(ANY, 888, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, 777)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(999), b32(888), b32(777)],
            box_references=boxes,
        )
    )
    assert result.abi_return is True

    # Clean up: forbid the global wildcard
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="forbid",
            args=[b32(ANY), b32(ANY), b32(ANY)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )


# ─── Forbid revokes wildcard ───

@pytest.mark.localnet
def test_forbid_wildcard(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """Revoking a wildcard entry removes the wildcard match."""
    app_id = guard_client.app_id

    # Forbid the sig wildcard for Alice → Treasury
    acl_box = _acl_box(SRC_ALICE, DST_TREASURY, ANY)
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="forbid",
            args=[b32(SRC_ALICE), b32(DST_TREASURY), b32(ANY)],
            box_references=[box_ref(app_id, acl_box)],
        )
    )

    # Now Alice should NOT be able to call Treasury.approve
    boxes = [
        box_ref(app_id, _acl_box(SRC_ALICE, DST_TREASURY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_ALICE, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(SRC_ALICE, ANY, ANY)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, DST_TREASURY, ANY)),
        box_ref(app_id, _acl_box(ANY, ANY, SIG_APPROVE)),
        box_ref(app_id, _acl_box(ANY, ANY, ANY)),
    ]

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="canCall",
            args=[b32(SRC_ALICE), b32(DST_TREASURY), b32(SIG_APPROVE)],
            box_references=boxes,
        )
    )
    assert result.abi_return is False


# ─── Multiple Independent Permits ───

@pytest.mark.localnet
def test_multiple_permits(
    guard_client: au.AppClient, account: SigningAccount
) -> None:
    """Multiple permits can coexist independently."""
    app_id = guard_client.app_id
    acl_box1 = _acl_box(SRC_BOB, DST_TREASURY, SIG_TRANSFER)
    acl_box2 = _acl_box(SRC_BOB, DST_TREASURY, SIG_APPROVE)

    # Permit Bob → Treasury for both transfer and approve
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box1)],
        )
    )

    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="permit",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box2)],
        )
    )

    # Both should be set
    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box1)],
        )
    )
    assert result.abi_return is True

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box2)],
        )
    )
    assert result.abi_return is True

    # Forbid just transfer — approve should remain
    guard_client.send.call(
        au.AppClientMethodCallParams(
            method="forbid",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box1)],
        )
    )

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_TRANSFER)],
            box_references=[box_ref(app_id, acl_box1)],
        )
    )
    assert result.abi_return is False

    result = guard_client.send.call(
        au.AppClientMethodCallParams(
            method="getAcl",
            args=[b32(SRC_BOB), b32(DST_TREASURY), b32(SIG_APPROVE)],
            box_references=[box_ref(app_id, acl_box2)],
        )
    )
    assert result.abi_return is True
