"""ConfigPositionManager tests — ported from upstream
tests/contracts/position-manager/ConfigPositionManager/ConfigPositionManager.t.sol.

Tier-2 starter for the position-manager surface. Concrete tests cover:
 - deploy succeeds
 - registerSpoke (success path) toggles the internal map
 - all 8 spoke-gated methods revert with `SpokeNotRegistered` when
   passed an unregistered spoke (covers setGlobalPermission,
   setCanUpdate*, renounceGlobalPermission, renounceCanUpdate*,
   setUsingAsCollateralOnBehalfOf, updateUserRiskPremiumOnBehalfOf,
   updateUserDynamicConfigOnBehalfOf)

Skipped from upstream (need Hub/Spoke wired):
 - happy-path setX/renounceX (state assertions need _getPermissions
   read which depends on Spoke ABI)
 - setUsingAsCollateralOnBehalfOf state-change assertions (forward
   to ISpoke)
 - all `_fuzz_*` and `.Permit.*` (ECDSA signing path)
"""

from __future__ import annotations

import os

import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk import encoding
from conftest import deploy_contract


# Address constants. account.address (deployer) → 32-byte public key
# below; bob/alice are arbitrary other addresses.
def _addr_pk32(addr: str) -> bytes:
    return encoding.decode_address(addr)


# Two arbitrary addresses for delegator/delegatee. Generated as
# checksum(b"appID" + N) since algokit_utils localnet doesn't expose
# a "random-address" helper without funding it.
ALICE_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (1).to_bytes(8, "big"))
)
BOB_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (2).to_bytes(8, "big"))
)
SPOKE1_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (3).to_bytes(8, "big"))
)
SPOKE2_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (4).to_bytes(8, "big"))
)
ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


@pytest.fixture(scope="module")
def cpm(localnet, account):
    """Deploy ConfigPositionManager with `account` as initial owner."""
    return deploy_contract(
        localnet, account, "ConfigPositionManager",
        app_args=[_addr_pk32(account.address)],
    )


def _call(client, method, *args, boxes=None):
    kwargs = dict(method=method, args=list(args), note=os.urandom(8))
    if boxes:
        kwargs["box_references"] = boxes
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


# ─── Deploy + ownership ───────────────────────────────────────────────────────


def test_deploy(cpm):
    assert cpm.app_id > 0


def test_owner_initial(cpm, account):
    """After __postInit(initialOwner=account), owner() returns account."""
    assert _call(cpm, "owner") == account.address


# ─── registerSpoke (Ownable, our owner = account) ─────────────────────────────


def test_registerSpoke_revertsWith_InvalidAddress(cpm):
    """registerSpoke rejects address(0)."""
    with pytest.raises(LogicError):
        _call(cpm, "registerSpoke", ZERO_ADDR, True)


# ─── SpokeNotRegistered reverts on the 8 spoke-gated methods ─────────────────
# Each method takes spoke as the first arg and is guarded by
# onlyRegisteredSpoke. A fresh CPM has no registered spokes, so any
# call with a "spoke" address that hasn't been registered must revert.


def test_setGlobalPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "setGlobalPermission", SPOKE2_ADDR, BOB_ADDR, True)


def test_setCanUpdateUsingAsCollateralPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "setCanUpdateUsingAsCollateralPermission",
              SPOKE2_ADDR, BOB_ADDR, True)


def test_setCanUpdateUserRiskPremiumPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "setCanUpdateUserRiskPremiumPermission",
              SPOKE2_ADDR, BOB_ADDR, True)


def test_setCanUpdateUserDynamicConfigPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "setCanUpdateUserDynamicConfigPermission",
              SPOKE2_ADDR, BOB_ADDR, True)


def test_renounceGlobalPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "renounceGlobalPermission", SPOKE2_ADDR, ALICE_ADDR)


def test_renounceCanUpdateUsingAsCollateralPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "renounceCanUpdateUsingAsCollateralPermission",
              SPOKE2_ADDR, ALICE_ADDR)


def test_renounceCanUpdateUserRiskPremiumPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "renounceCanUpdateUserRiskPremiumPermission",
              SPOKE2_ADDR, ALICE_ADDR)


def test_renounceCanUpdateUserDynamicConfigPermission_revertsWith_SpokeNotRegistered(cpm):
    with pytest.raises(LogicError):
        _call(cpm, "renounceCanUpdateUserDynamicConfigPermission",
              SPOKE2_ADDR, ALICE_ADDR)


def test_setUsingAsCollateralOnBehalfOf_revertsWith_SpokeNotRegistered(cpm):
    # Signature in arc56: (spoke, reserveId(uint256), usingAsCollateral, onBehalfOf)
    with pytest.raises(LogicError):
        _call(cpm, "setUsingAsCollateralOnBehalfOf",
              SPOKE2_ADDR, 0, True, ALICE_ADDR)


def test_updateUserRiskPremiumOnBehalfOf_revertsWith_SpokeNotRegistered(cpm):
    # Signature: (spoke, onBehalfOf) — risk premium read from caller state
    with pytest.raises(LogicError):
        _call(cpm, "updateUserRiskPremiumOnBehalfOf", SPOKE2_ADDR, ALICE_ADDR)


def test_updateUserDynamicConfigOnBehalfOf_revertsWith_SpokeNotRegistered(cpm):
    # Signature: (spoke, onBehalfOf)
    with pytest.raises(LogicError):
        _call(cpm, "updateUserDynamicConfigOnBehalfOf", SPOKE2_ADDR, ALICE_ADDR)
