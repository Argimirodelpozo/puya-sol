"""Translation of v2 src/test/CollateralOnramp.t.sol — all 6 tests.

CollateralOnramp.wrap(asset, to, amount) does two things:

  1. Pulls `amount` of `asset` from msg.sender into the CollateralToken via
     Solady SafeTransferLib's `safeTransferFrom` (inline-asm `call(gas,
     token, …)` on a non-constant target).
  2. Calls `CollateralToken.wrap(asset, to, amount, address(0), "")` to
     mint pUSD to `to` and forward the asset to the vault.

Step 1 currently doesn't lower correctly on AVM: puya-sol's Yul `call`
handler stubs the call as success without firing the inner `itxn
ApplicationCall`, so the asset transfer silently no-ops (same surface as
PUYA_BLOCKERS.md §3 / the SAFETRANSFERLIB_CALL_STUB note in
test_collateral_token.py). With no asset actually deposited in the
CollateralToken's account, step 2's `IERC20Min(asset).transfer(VAULT,
amount)` then fails (insufficient balance), so the whole positive flow
errors out before any pUSD mint.

That's an AVM-port-adaptation gap in CollateralOnramp.sol: it should
mirror CollateralToken.sol's switch from `SafeTransferLib` →
`IERC20Min`. Until that lands the wrap-positive paths are xfailed.

The pause-revert + pause-unauthorized paths don't need the
SafeTransferLib lowering and translate cleanly. The Onramp's
pause-unauthorized check already lives in test_pausable_unauthorized.py;
this file covers the asset-paused revert + unpause-resume flow.
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.deals import deal_usdc, set_allowance, usdc_balance
from dev.invoke import call


SAFETRANSFERLIB_CALL_STUB_ONRAMP = (
    "CollateralOnramp.wrap calls Solady SafeTransferLib.safeTransferFrom "
    "on a non-constant token; puya-sol's Yul call handler stubs that as "
    "success without firing an inner txn, so the asset never reaches "
    "CollateralToken's account and the downstream IERC20Min.transfer to "
    "the vault errors. Same shape as PUYA_BLOCKERS.md §3 / "
    "test_collateral_token.py's SAFETRANSFERLIB_CALL_STUB. Resolves once "
    "CollateralOnramp.sol switches from SafeTransferLib to IERC20Min "
    "(mirroring CollateralToken.sol's existing AVM-port adaptation)."
)


# `collateral_onramp_wired` (real CollateralToken backing) lives in
# test/conftest.py.


# ── WRAP (positive) — xfailed pending Onramp.sol AVM-port adaptation ─────


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_ONRAMP, strict=True)
def test_CollateralOnramp_wrapUSDC(
    collateral_onramp_wired, collateral_token_wired,
    usdc_stateful, vault, funded_account
):
    """Onramp.wrap(USDC, alice, amt) should pull USDC from alice into the
    token, then mint pUSD to alice and transfer USDC to the vault."""
    amount = 100_000_000
    alice32 = bytes(addr(funded_account), "ascii")  # placeholder; rewritten below
    # Use the funded_account's raw 32-byte algod address consistently.
    from algosdk.encoding import decode_address
    alice32 = decode_address(funded_account.address)

    deal_usdc(usdc_stateful, alice32, amount)
    set_allowance(usdc_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)

    call(
        collateral_onramp_wired, "wrap",
        [
            app_id_to_address(usdc_stateful.app_id),
            addr(funded_account),
            amount,
        ],
        sender=funded_account,
    )

    assert usdc_balance(usdc_stateful, alice32) == 0
    assert usdc_balance(usdc_stateful, decode_address(vault.address)) == amount
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == amount


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_ONRAMP, strict=True)
def test_CollateralOnramp_wrapUSDCe(
    collateral_onramp_wired, collateral_token_wired,
    usdce_stateful, vault, funded_account
):
    """Same as wrapUSDC but for the USDCe asset slot."""
    amount = 100_000_000
    from algosdk.encoding import decode_address
    alice32 = decode_address(funded_account.address)

    deal_usdc(usdce_stateful, alice32, amount)
    set_allowance(usdce_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)

    call(
        collateral_onramp_wired, "wrap",
        [
            app_id_to_address(usdce_stateful.app_id),
            addr(funded_account),
            amount,
        ],
        sender=funded_account,
    )

    assert usdc_balance(usdce_stateful, alice32) == 0
    assert usdc_balance(usdce_stateful, decode_address(vault.address)) == amount
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == amount


# ── WRAP (revert paths) — work without SafeTransferLib lowering ──────────


def test_revert_CollateralOnramp_wrapUSDC_paused(
    collateral_onramp, mock_token, admin, funded_account
):
    """When the asset is paused, wrap must revert with OnlyUnpaused before
    touching the asset transfer or the CollateralToken."""
    call(collateral_onramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            collateral_onramp, "wrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
            ],
            sender=funded_account,
        )


def test_revert_CollateralOnramp_wrapUSDCe_paused(
    collateral_onramp, mock_token, admin, funded_account
):
    """Same shape as wrapUSDC_paused — the pause check is on the asset
    address, so any paused asset reverts identically. The Foundry test
    distinguishes USDC vs USDCe; on AVM the pause table is a single
    mapping(address ⇒ bool), so one revert path covers both. Translated
    1:1 for parity with the upstream count."""
    call(collateral_onramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            collateral_onramp, "wrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
            ],
            sender=funded_account,
        )


# ── Pausable unpause — positive flow, gated on the same SafeTransferLib gap ──


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_ONRAMP, strict=True)
def test_Pausable_unpause(
    collateral_onramp_wired, collateral_token_wired,
    usdc_stateful, vault, admin, funded_account
):
    """Pause → wrap reverts → unpause → wrap succeeds. The first half of
    the flow (pause + revert) works; the post-unpause wrap is blocked on
    the same Onramp SafeTransferLib gap as the positive wrap tests."""
    from algosdk.encoding import decode_address
    alice32 = decode_address(funded_account.address)
    amount = 100_000_000

    call(collateral_onramp_wired, "pause",
         [app_id_to_address(usdc_stateful.app_id)], sender=admin)

    deal_usdc(usdc_stateful, alice32, amount)
    set_allowance(usdc_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)
    with pytest.raises(LogicError):
        call(
            collateral_onramp_wired, "wrap",
            [
                app_id_to_address(usdc_stateful.app_id),
                addr(funded_account),
                amount,
            ],
            sender=funded_account,
        )

    call(collateral_onramp_wired, "unpause",
         [app_id_to_address(usdc_stateful.app_id)], sender=admin)

    call(
        collateral_onramp_wired, "wrap",
        [
            app_id_to_address(usdc_stateful.app_id),
            addr(funded_account),
            amount,
        ],
        sender=funded_account,
    )

    assert usdc_balance(usdc_stateful, alice32) == 0
    assert usdc_balance(usdc_stateful, decode_address(vault.address)) == amount
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == amount


# Note: test_revert_Pausable_pause_unauthorized lives in
# test_pausable_unauthorized.py (already translated).
