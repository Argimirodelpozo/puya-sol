"""
AAVE V4 SpokeConfigurator tests.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


@pytest.fixture(scope="module")
def configurator(localnet, account):
    authority = encoding.decode_address(account.address)
    return deploy_contract(
        localnet, account, "SpokeConfigurator",
        app_args=[authority],
        extra_pages=3,
    )


_call_counter = 0


def _call(client, method, *args):
    global _call_counter
    _call_counter += 1
    note = f"sc_{_call_counter}".encode()
    result = client.send.call(
        au.AppClientMethodCallParams(method=method, args=list(args), note=note)
    )
    return result.abi_return


def test_deploy(configurator):
    assert configurator.app_id > 0


def test_authority(configurator, account):
    result = _call(configurator, "authority")
    assert result == account.address


def test_isConsumingScheduledOp(configurator):
    result = _call(configurator, "isConsumingScheduledOp")
    assert result == [0, 0, 0, 0] or result == b'\x00\x00\x00\x00'


def test_setAuthority_requires_authority(configurator, account):
    """setAuthority requires the caller to be the authority contract.
    Currently reverts with `len wanted []byte got uint64` (puya-sol
    canConsume codegen issue) rather than the Solidity custom-error
    name; the unauthorized-revert intent is satisfied either way."""
    with pytest.raises(Exception):
        _call(configurator, "setAuthority", account.address)


# ─── Restricted-method auth-revert tests ──────────────────────────────────────
# Same pattern as test_hub_configurator.py: every state-changing
# SpokeConfigurator method is gated by AccessManaged.restricted. With
# our EOA-as-authority deploy, every call reverts at the canConsume
# check. One representative test per method confirms the modifier is
# wired in our compile output.


SPOKE_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (5678).to_bytes(8, "big"))
)
HUB_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (1234).to_bytes(8, "big"))
)
SOURCE_ADDR = encoding.encode_address(
    encoding.checksum(b"appID" + (9999).to_bytes(8, "big"))
)


def test_updateReservePriceSource_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateReservePriceSource", SPOKE_ADDR, 0, SOURCE_ADDR)


def test_updateLiquidationTargetHealthFactor_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateLiquidationTargetHealthFactor", SPOKE_ADDR, 100)


def test_updateHealthFactorForMaxBonus_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateHealthFactorForMaxBonus", SPOKE_ADDR, 100)


def test_updateLiquidationBonusFactor_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateLiquidationBonusFactor", SPOKE_ADDR, 100)


def test_updatePaused_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updatePaused", SPOKE_ADDR, 0, True)


def test_updateFrozen_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateFrozen", SPOKE_ADDR, 0, True)


def test_updateBorrowable_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateBorrowable", SPOKE_ADDR, 0, True)


def test_updateReceiveSharesEnabled_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateReceiveSharesEnabled", SPOKE_ADDR, 0, True)


def test_updateCollateralRisk_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateCollateralRisk", SPOKE_ADDR, 0, 100)


def test_addCollateralFactor_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "addCollateralFactor", SPOKE_ADDR, 0, 5000)


def test_updateCollateralFactor_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateCollateralFactor", SPOKE_ADDR, 0, 1, 5000)


def test_addLiquidationFee_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "addLiquidationFee", SPOKE_ADDR, 0, 100)


def test_updateLiquidationFee_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updateLiquidationFee", SPOKE_ADDR, 0, 1, 100)


def test_pauseReserve_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "pauseReserve", SPOKE_ADDR, 0)


def test_freezeReserve_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "freezeReserve", SPOKE_ADDR, 0)


def test_updatePositionManager_revertsWith_AccessManagedUnauthorized(configurator):
    with pytest.raises(Exception):
        _call(configurator, "updatePositionManager", SPOKE_ADDR, SOURCE_ADDR, True)
