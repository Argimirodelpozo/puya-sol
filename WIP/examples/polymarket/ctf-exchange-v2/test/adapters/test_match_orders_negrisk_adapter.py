"""Translation of v2 src/test/MatchOrdersNegRiskCtfCollateralAdapter.t.sol —
8 tests.

Mirror of MatchOrdersCtfCollateralAdapter, but with NegRiskAdapter.

Same dependency chain as the non-NegRisk adapter case (see
test_match_orders_ctf_adapter.py XFAIL_NEEDS_ADAPTER_FIXTURES). The
NegRisk variant additionally needs `getPositionId` / `convertPositions`
inner-call surface from the CTFMock or NegRiskAdapter mock.
"""
import pytest


XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES = (
    "Needs (1) `split_exchange_with_adapter` fixture wired through "
    "NegRiskCtfCollateralAdapter, (2) maker funding via Onramp.wrap (which "
    "in turn needs the Solady CT.transferFrom storage-slot bug fixed — see "
    "test_collateral_offramp.py), and (3) NegRiskAdapter inner-call "
    "wiring (`getPositionId` / `convertPositions`) on the CTFMock."
)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint_Fees():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Complementary():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Complementary_Fees():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge_Fees():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge_Reverts_WhenAdapterNotApproved():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint_Reverts_WhenAdapterUsdceMismatch():
    raise NotImplementedError(XFAIL_NEEDS_NEGRISK_ADAPTER_FIXTURES)
