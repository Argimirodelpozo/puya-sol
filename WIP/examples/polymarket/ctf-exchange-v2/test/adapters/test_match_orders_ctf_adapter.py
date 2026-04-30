"""Translation of v2 src/test/MatchOrdersCtfCollateralAdapter.t.sol — 8 tests.

Exercises matchOrders settlement when:
  - collateral slot      = CollateralToken (pUSD)
  - outcomeTokenFactory  = CtfCollateralAdapter
  - ctfCollateral        = USDCe

`split_exchange_with_adapter` and `split_adapter_with_delegate` fixtures
are wired up in conftest.py and ready to use. Two layered blockers
remain before any of the 8 cases can run:

1. **uint256 / uint512 approve selector** — RESOLVED. Solady's open-coded
   approve compiled to `approve(address,uint512)bool` while
   `Assets.ctor` calls it with the canonical `approve(address,uint256)bool`
   selector. Fixed by routing approve through `_approve` in Solady
   (matches the existing transferFrom patch) and adding an `approve`
   entry to `IERC20Min` in TransferHelper.sol so Assets dispatches via
   the uint256 shape.

2. **Maker funding via real CollateralToken** — OPEN. Order makers in
   matchOrders use eth-style identities (bob/carla, ECDSA secp256k1)
   with no algorand key, so `dealUsdcAndApprove`-style cheats can't be
   used directly: real Solady CollateralToken has no test cheat to set
   allowances, and bob can't sign `pUSD.approve(h1)` himself.
   `preapproveOrder` doesn't help — `_preapproveOrder` itself runs
   `_isValidSignature`, so the order still needs a real ECDSA signature.
   Three feasible paths once we tackle this:
     - EIP-2612 `permit`: bob signs an off-chain permit, anyone submits;
       requires Solady's inline-asm permit to make it through puya-sol's
       optimizer (the same hazard that broke `ECDSA.recoverCalldata`).
     - ERC-1271 maker: deploy a mock 1271 wallet that holds pUSD,
       approves h1 from admin, and validates signatures unconditionally.
       Order's `maker` becomes the mock's address.
     - CollateralToken test cheat: add admin-gated `__test_setAllowance`
       to CT (production change, but minimal AVM-port pattern).
"""
import pytest


XFAIL_MAKER_FUNDING = (
    "Blocked on maker funding for real CollateralToken (pUSD): bob/carla "
    "have eth ECDSA keys but no algorand keys, so they can't sign "
    "`CT.approve(h1, _)` directly. Solady CT has no test cheat. "
    "preapproveOrder doesn't bypass — _preapproveOrder still runs "
    "_isValidSignature. Need EIP-2612 permit, an ERC-1271 maker mock, "
    "or a test cheat on CT. uint256↔uint512 approve selector mismatch "
    "(separate issue) is now resolved via the Solady approve patch."
)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint_Fees():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Complementary():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Complementary_Fees():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge_Fees():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Merge_Reverts_WhenAdapterNotApproved():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)


@pytest.mark.xfail(reason=XFAIL_MAKER_FUNDING, strict=True)
def test_MatchOrdersCtfCollateralAdapter_Mint_Reverts_WhenAdapterUsdceMismatch():
    raise NotImplementedError(XFAIL_MAKER_FUNDING)
