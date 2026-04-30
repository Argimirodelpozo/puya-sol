"""Pausable / role-revert tests for the standalone collateral contracts.

These all check the negative path — a non-owner / non-witness / non-admin
caller invoking a privileged method must revert. Even with the broken
ownership state from PUYA_BLOCKERS.md §1 (owner reads back as zero address),
these tests pass because every sender is "not the (zero) owner."

Foundry sources:
  * src/test/CollateralOnramp.t.sol::test_revert_Pausable_pause_unauthorized
  * src/test/CollateralOfframp.t.sol::test_revert_Pausable_pause_unauthorized
  * src/test/CtfCollateralAdapter.t.sol::test_revert_CtfCollateralAdapter_pause_unauthorized
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import app_id_to_address
from dev.invoke import call


def test_revert_CollateralOnramp_pause_unauthorized(collateral_onramp, mock_token, funded_account):
    """A non-admin call to onramp.pause(asset) must revert."""
    with pytest.raises(LogicError):
        call(
            collateral_onramp, "pause",
            [app_id_to_address(mock_token.app_id)],
            sender=funded_account,
        )


def test_revert_CollateralOfframp_pause_unauthorized(collateral_offramp, mock_token, funded_account):
    """A non-admin call to offramp.pause(asset) must revert."""
    with pytest.raises(LogicError):
        call(
            collateral_offramp, "pause",
            [app_id_to_address(mock_token.app_id)],
            sender=funded_account,
        )


def test_revert_PermissionedRamp_pause_unauthorized(permissioned_ramp, mock_token, funded_account):
    """A non-admin call to permissioned_ramp.pause(asset) must revert."""
    with pytest.raises(LogicError):
        call(
            permissioned_ramp, "pause",
            [app_id_to_address(mock_token.app_id)],
            sender=funded_account,
        )
