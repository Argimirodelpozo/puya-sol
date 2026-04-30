"""Translation of v2 src/test/NegRiskCtfCollateralAdapter.t.sol — 12 tests.

Mirror of CtfCollateralAdapter tests, plus NegRisk-specific
`convertPositions` flow (one/two/three NO→YES, with fees, zero amount).

The positive flows pull pUSD via Solady CT.transferFrom which has the
same storage-slot bug that blocks test_collateral_offramp.py — xfailed
under one umbrella reason. The pause-revert paths fire at
`onlyUnpaused(USDCE)` *before* any transferFrom, so they translate
cleanly.
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, app_id_to_address
from dev.invoke import call


_SOLADY_CT_TRANSFERFROM = (
    "split/merge/redeem/convertPositions pull pUSD via Solady CT.transferFrom; "
    "same storage-slot bug as test_collateral_offramp.py."
)


# ── splitPosition / mergePositions / redeemPositions ─────────────────────


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_splitPosition(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_mergePositions(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
@pytest.mark.parametrize("outcome", [True, False],
                         ids=["yes", "no"])
def test_NegRiskCtfCollateralAdapter_redeemPositions(negrisk_adapter, outcome):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


# ── convertPositions (NegRisk-specific) ──────────────────────────────────


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_oneNoToYes(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_twoNoToYes(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_threeNoToYes(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_withFees(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


@pytest.mark.xfail(reason=_SOLADY_CT_TRANSFERFROM, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_zeroAmount(negrisk_adapter):
    raise NotImplementedError(_SOLADY_CT_TRANSFERFROM)


# ── PAUSE-REVERT paths — fire at onlyUnpaused before any transferFrom ────


def test_revert_NegRiskCtfCollateralAdapter_splitPosition_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    """splitPosition reverts at onlyUnpaused(USDCE) modifier."""
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            negrisk_adapter, "splitPosition",
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32,
             [1, 2], 100_000_000],
            sender=funded_account,
        )


def test_revert_NegRiskCtfCollateralAdapter_mergePositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    """mergePositions reverts at onlyUnpaused."""
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            negrisk_adapter, "mergePositions",
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32,
             [1, 2], 100_000_000],
            sender=funded_account,
        )


def test_revert_NegRiskCtfCollateralAdapter_redeemPositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    """redeemPositions reverts at onlyUnpaused."""
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            negrisk_adapter, "redeemPositions",
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2]],
            sender=funded_account,
        )


def test_revert_NegRiskCtfCollateralAdapter_convertPositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    """convertPositions reverts at onlyUnpaused."""
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            negrisk_adapter, "convertPositions",
            [b"\x00" * 32, 1, 100_000_000],
            sender=funded_account,
        )
