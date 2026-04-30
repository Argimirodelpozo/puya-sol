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


ONRAMP_ADDR_CONVENTION_MISMATCH = (
    "AVM-port-adapted CollateralOnramp.wrap (now using IERC20Min instead "
    "of Solady SafeTransferLib) lowers to a real itxn, but the no-callback "
    "wrap chain has a deeper representation issue: Solidity stores "
    "`address(COLLATERAL_TOKEN)` as the puya-sol convention bytes "
    "(`\\x00*24 + itob(app_id)`), so Onramp's "
    "`IERC20Min(asset).transferFrom(msg.sender, COLLATERAL_TOKEN, amt)` "
    "credits the asset under the puya-sol-convention key. Then CT's "
    "downstream `IERC20Min(asset).transfer(VAULT, amt)` runs as an inner "
    "txn whose `Txn.Sender` is CT's algod-derived address "
    "(`sha512_256(\"appID\"||app_id)`), so USDCMock debits a *different* "
    "balance key — and CT has zero in the algod-addr ledger entry. "
    "Resolving this needs a coherent address-of-other-app representation "
    "across (a) inner-tx ApplicationID extraction and (b) arg-passing — "
    "today puya-sol picks the puya-sol convention for both, which works "
    "for ApplicationID (low-8-byte extraction) but not for arg-passing "
    "(`Txn.Sender` resolves to algod-derived). Tracked separately."
)


# `collateral_onramp_wired` (real CollateralToken backing) lives in
# test/conftest.py.


# ── WRAP (positive) — xfailed pending Onramp.sol AVM-port adaptation ─────


@pytest.mark.xfail(reason=ONRAMP_ADDR_CONVENTION_MISMATCH, strict=True)
def test_CollateralOnramp_wrapUSDC(
    collateral_onramp_wired, collateral_token_wired,
    usdc_stateful, vault, funded_account
):
    """Onramp.wrap(USDC, alice, amt) should pull USDC from alice into the
    token, then mint pUSD to alice and transfer USDC to the vault."""
    from algosdk.encoding import decode_address
    amount = 100_000_000
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


@pytest.mark.xfail(reason=ONRAMP_ADDR_CONVENTION_MISMATCH, strict=True)
def test_CollateralOnramp_wrapUSDCe(
    collateral_onramp_wired, collateral_token_wired,
    usdce_stateful, vault, funded_account
):
    """Same as wrapUSDC but for the USDCe asset slot."""
    from algosdk.encoding import decode_address
    amount = 100_000_000
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


@pytest.mark.xfail(reason=ONRAMP_ADDR_CONVENTION_MISMATCH, strict=True)
def test_Pausable_unpause(
    collateral_onramp_wired, collateral_token_wired,
    usdc_stateful, vault, admin, funded_account
):
    """Pause → wrap reverts → unpause → wrap succeeds. After the
    AVM-port adaptation switched Onramp from SafeTransferLib to
    IERC20Min, the post-unpause wrap actually transfers."""
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
