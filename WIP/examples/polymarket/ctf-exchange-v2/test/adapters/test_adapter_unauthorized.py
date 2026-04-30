"""Pausable / role-revert tests for the standalone adapter contracts.

Mirrors `src/test/CtfCollateralAdapter.t.sol::test_revert_CtfCollateralAdapter_pause_unauthorized`
plus equivalents on the NegRisk variant once it deploys.

Limited to negative-path checks — see PUYA_BLOCKERS.md §1: the positive
ownership path requires the puya backend's ABI router fix.
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import app_id_to_address
from dev.invoke import call


def test_revert_CtfCollateralAdapter_pause_unauthorized(ctf_adapter, mock_token, funded_account):
    """A non-admin call to ctfAdapter.pause(asset) must revert."""
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "pause",
            [app_id_to_address(mock_token.app_id)],
            sender=funded_account,
        )
