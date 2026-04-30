"""Translation of v2 src/test/CtfCollateralAdapter.t.sol — 7 tests
(the 8th, pause_unauthorized, lives in test_adapter_unauthorized.py).

CtfCollateralAdapter.{splitPosition,mergePositions,redeemPositions}
bridges CollateralToken ↔ ConditionalTokens. Each call:

  1. (split) Pulls pUSD from msg.sender via Solady CT.transferFrom, then
     CT.unwrap(USDCE, this, ...) to get USDCE, then CTF.splitPosition.
  2. (merge) Reverse of split — CTF.mergePositions, then re-wrap USDCE
     into pUSD.
  3. (redeem) After payouts reported, CTF.redeemPositions, then re-wrap.

Steps (1) and (2) hit the same Solady CT.transferFrom storage-slot bug
that blocks `test_collateral_offramp.py`'s unwrap-positive paths
(addresses get hashed to a slot inconsistent with what _mint/_burn use).
Until that lands, the positive split/merge/redeem flows xfail under
one umbrella.

The pause-revert paths CAN be tested cleanly: the adapter's
`onlyUnpaused(USDCE)` modifier fires at the *start* of every entry
point, before any transferFrom — so a paused-asset call reverts
immediately without touching the broken Solady storage path.
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, app_id_to_address
from dev.invoke import call


_SOLADY_CT_TRANSFERFROM = (
    "splitPosition/mergePositions/redeemPositions pull pUSD from sender "
    "via Solady CT.transferFrom, which has a storage-slot derivation bug "
    "on AVM (transferFrom reads a different slot than _mint wrote). "
    "Tracked as the same gap in test_collateral_offramp.py."
)


# ── splitPosition / mergePositions / redeemPositions positive paths ──────


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_CtfCollateralAdapter_splitPosition(ctf_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_CtfCollateralAdapter_mergePositions(ctf_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
@pytest.mark.parametrize("outcome", [True, False],
                         ids=["yes", "no"])
def test_CtfCollateralAdapter_redeemPositions(ctf_adapter, outcome):
    """Foundry test takes a `bool _outcome` fuzz arg; Forge runs both."""
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


# ── PAUSE-REVERT paths — fire at onlyUnpaused before any transferFrom ────


def test_revert_CtfCollateralAdapter_splitPosition_paused(
    ctf_adapter, mock_token, admin, funded_account
):
    """When USDCE is paused on the adapter, splitPosition reverts at the
    `onlyUnpaused(USDCE)` modifier — fires before pulling pUSD or
    touching the CTF mock."""
    call(ctf_adapter, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "splitPosition",
            [
                b"\x00" * 32,                        # collateralToken (unused)
                b"\x00" * 32,                        # parentCollectionId (bytes32)
                b"\x00" * 32,                        # conditionId (bytes32)
                [1, 2],                              # partition
                100_000_000,                         # amount
            ],
            sender=funded_account,
        )


def test_revert_CtfCollateralAdapter_mergePositions_paused(
    ctf_adapter, mock_token, admin, funded_account
):
    """Mirror of splitPosition_paused for mergePositions."""
    call(ctf_adapter, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "mergePositions",
            [
                b"\x00" * 32, b"\x00" * 32, b"\x00" * 32,
                [1, 2],
                100_000_000,
            ],
            sender=funded_account,
        )


def test_revert_CtfCollateralAdapter_redeemPositions_paused(
    ctf_adapter, mock_token, admin, funded_account
):
    """redeemPositions also gates on onlyUnpaused."""
    call(ctf_adapter, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "redeemPositions",
            [
                b"\x00" * 32, b"\x00" * 32, b"\x00" * 32,
                [1, 2],
            ],
            sender=funded_account,
        )


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_CtfCollateralAdapter_unpause(ctf_adapter):
    """Pause → splitPosition reverts → unpause → splitPosition succeeds.
    The post-unpause splitPosition needs a working CT.transferFrom."""
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


# Note: test_revert_CtfCollateralAdapter_pause_unauthorized lives in
# test_adapter_unauthorized.py (already translated).
