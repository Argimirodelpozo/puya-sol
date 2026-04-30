"""Translation of v2 src/test/CtfCollateralAdapter.t.sol — 7 tests
(the 8th, pause_unauthorized, lives in test_adapter_unauthorized.py).

CtfCollateralAdapter.{splitPosition,mergePositions,redeemPositions}
bridges CollateralToken ↔ ConditionalTokens. Each call:

  1. (split / merge) Pulls pUSD from msg.sender (or burns/mints
     position tokens) via Solady SafeTransferLib + IConditionalTokens
     inner-calls.
  2. Calls into the wired CTF mock (`splitPosition`, `mergePositions`,
     `redeemPositions`).

The whole positive surface depends on the same pre-step chain as the
PermissionedRamp/Onramp/Offramp tests:

  * Mint USDCe to alice → wrap into pUSD → approve adapter → call
    adapter.splitPosition.

The wrap step hits the SAFETRANSFERLIB_CALL_STUB_ONRAMP gap, so even
the pause-revert tests (which deposit pUSD before triggering the
revert) can't reach their `expectRevert` cleanly. They're marked xfail
under one umbrella reason. The single pure pause-unauthorized test
(no wrap chain) is in test_adapter_unauthorized.py and passes today.
"""
import pytest


XFAIL_NEEDS_ONRAMP = (
    "CtfCollateralAdapter tests require a working CollateralOnramp.wrap "
    "to pre-fund alice with pUSD; the Onramp wrap is itself blocked on "
    "the Solady SafeTransferLib lowering gap (see "
    "test_collateral_onramp.py SAFETRANSFERLIB_CALL_STUB_ONRAMP). The "
    "downstream adapter logic itself (splitPosition / mergePositions / "
    "redeemPositions inner-calls into CTFMock) is wired in conftest.py "
    "but not exercised here until the wrap precondition lands."
)


# ── splitPosition / mergePositions / redeemPositions positive paths ──────
# All five depend on the wrap chain.


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_CtfCollateralAdapter_splitPosition(ctf_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_CtfCollateralAdapter_mergePositions(ctf_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
@pytest.mark.parametrize("outcome", [True, False],
                         ids=["yes", "no"])
def test_CtfCollateralAdapter_redeemPositions(ctf_adapter, outcome):
    """The Foundry test takes a `bool _outcome` fuzz arg; `forge test`
    runs both true and false. Translated as a parametrize."""
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


# ── PAUSE-REVERT paths — also need the wrap chain to set up state ────────


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_CtfCollateralAdapter_splitPosition_paused(ctf_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_CtfCollateralAdapter_mergePositions_paused(ctf_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_CtfCollateralAdapter_redeemPositions_paused(ctf_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_CtfCollateralAdapter_unpause(ctf_adapter):
    """Pause → splitPosition reverts → unpause → splitPosition succeeds.
    Same wrap-chain dependency."""
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


# Note: test_revert_CtfCollateralAdapter_pause_unauthorized lives in
# test_adapter_unauthorized.py (already translated).
