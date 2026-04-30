"""Translation of v2 src/test/NegRiskCtfCollateralAdapter.t.sol — 12 tests.

Mirror of CtfCollateralAdapter tests, plus NegRisk-specific
`convertPositions` flow (one/two/three NO→YES, with fees, zero amount).

All tests depend on:

  * The Onramp wrap chain (mint USDCe → wrap → pUSD).
  * The NegRiskAdapter inner-call surface for splitPosition /
    mergePositions / convertPositions / redeemPositions.

The Onramp wrap leg is blocked on the SafeTransferLib gap (see
test_collateral_onramp.py SAFETRANSFERLIB_CALL_STUB_ONRAMP). Until that
lands the whole NegRisk surface stays xfailed.

The pure pause-unauthorized check could land independently as a
test_adapter_unauthorized.py-style test against the `negrisk_adapter`
fixture — TODO when the Foundry-source has one (it does NOT in this
file; the unauthorized test is on the parent CtfCollateralAdapter).
"""
import pytest


XFAIL_NEEDS_ONRAMP = (
    "NegRiskCtfCollateralAdapter tests require a working "
    "CollateralOnramp.wrap chain to seed alice with pUSD. The Onramp "
    "wrap is blocked on the Solady SafeTransferLib lowering gap (see "
    "test_collateral_onramp.py SAFETRANSFERLIB_CALL_STUB_ONRAMP). "
    "Beyond the wrap precondition, the NegRisk-specific "
    "`convertPositions` flow also needs the NegRiskAdapter inner-call "
    "(`getPositionId` / `convertPositions`) plumbing in delegate/"
    "ctf_mock.py — also TODO."
)


# ── splitPosition / mergePositions / redeemPositions ─────────────────────


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_splitPosition(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_mergePositions(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
@pytest.mark.parametrize("outcome", [True, False],
                         ids=["yes", "no"])
def test_NegRiskCtfCollateralAdapter_redeemPositions(negrisk_adapter, outcome):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


# ── convertPositions (NegRisk-specific) ──────────────────────────────────


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_oneNoToYes(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_twoNoToYes(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_threeNoToYes(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_withFees(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_zeroAmount(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


# ── PAUSE-REVERT paths ───────────────────────────────────────────────────


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_NegRiskCtfCollateralAdapter_splitPosition_paused(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_NegRiskCtfCollateralAdapter_mergePositions_paused(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_NegRiskCtfCollateralAdapter_redeemPositions_paused(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ONRAMP, strict=True)
def test_revert_NegRiskCtfCollateralAdapter_convertPositions_paused(negrisk_adapter):
    raise NotImplementedError(XFAIL_NEEDS_ONRAMP)
