"""Translation of v2 src/test/MatchOrdersCtfCollateralAdapter.t.sol — 8 tests.

Exercises the matchOrders flow when the exchange's collateral slot is
the wrapped CollateralToken and the CTF-side bridging goes through
CtfCollateralAdapter (rather than the direct ConditionalTokens hookup).

All tests sit on top of the existing matchOrders pipeline (split
exchange + helper1/2 + lonely-chunk delegate). The 8 cases:
  * Mint, Mint_Fees
  * Complementary, Complementary_Fees
  * Merge, Merge_Fees
  * Merge_Reverts_WhenAdapterNotApproved
  * Mint_Reverts_WhenAdapterUsdceMismatch

The split CTFExchange + delegate plumbing already runs (see
test_match_orders.py — most of those tests pass after the 2026-04-30
puya bump). Adding the adapter dimension on top requires two new
fixtures:

  1. A `split_exchange_with_adapter` variant whose collateral/ctf slots
     point at a deployed CtfCollateralAdapter wired to the same wired
     CollateralToken / CTFMock as the rest of the orchestrator.
  2. Extra setup to fund alice with pUSD (via Onramp.wrap) instead of
     bare USDC, since the adapter expects the CollateralToken not the
     raw asset. This pulls in the same Onramp SafeTransferLib gap.

Until those fixtures land, the file is xfail-stubbed for the 8
adapter-dimension tests. The matchOrders core flow is covered by the
non-adapter test_match_orders.py.
"""
import pytest


XFAIL_NEEDS_ADAPTER_FIXTURES = (
    "Needs (1) split_exchange wired through CtfCollateralAdapter for "
    "collateral/CTF slots, and (2) the Onramp wrap chain to fund makers "
    "with pUSD. The latter is blocked on the Solady SafeTransferLib gap "
    "(see test_collateral_onramp.py SAFETRANSFERLIB_CALL_STUB_ONRAMP). "
    "Until both land, the matchOrders-via-adapter dimension stays as "
    "stub xfails on top of the matchOrders core (which passes today in "
    "test_match_orders.py)."
)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint_Fees():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Complementary():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Complementary_Fees():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge_Fees():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge_Reverts_WhenAdapterNotApproved():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)


@pytest.mark.xfail(reason=XFAIL_NEEDS_ADAPTER_FIXTURES, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint_Reverts_WhenAdapterUsdceMismatch():
    raise NotImplementedError(XFAIL_NEEDS_ADAPTER_FIXTURES)
