"""Translation of v2 src/test/CollateralOfframp.t.sol — all 6 tests.

CollateralOfframp.unwrap(asset, to, amount) is the inverse of
CollateralOnramp.wrap:

  1. Pulls `amount` pUSD from msg.sender into the CollateralToken via
     CT.transferFrom (Solady ERC20).
  2. Calls `CollateralToken.unwrap(asset, to, amount, address(0), "")`
     to burn that pUSD and transfer the underlying asset from the vault
     back to `to`.

After the AVM-port-adapted Solady ERC20 (transferFrom now routes through
_spendAllowance + _transfer instead of its inline assembly fast path),
the full unwrap chain works.
"""
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.invoke import call


# `collateral_offramp_wired` (real CollateralToken backing, with
# MINTER_ROLE + WRAPPER_ROLE granted) lives in test/conftest.py.


# ── UNWRAP (positive) — xfailed pending Offramp.sol AVM-port adaptation ──


def test_CollateralOfframp_unwrapUSDC(
    collateral_onramp_wired, collateral_offramp_wired,
    collateral_token_wired, usdc_stateful, vault, funded_account
):
    """Wrap USDC → pUSD via Onramp, then Offramp.unwrap pUSD → USDC.
    Final state: alice has the USDC back, vault holds none, alice's pUSD
    balance is zero."""
    from algosdk.encoding import decode_address
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    alice32 = decode_address(funded_account.address)
    vault32 = decode_address(vault.address)
    ct32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    amount = 100_000_000

    # Wrap leg
    deal_usdc(usdc_stateful, alice32, amount)
    set_allowance(usdc_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)
    call(collateral_onramp_wired, "wrap",
         [app_id_to_address(usdc_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    # Vault approves CT to pull the underlying USDC during the unwrap leg.
    # CT.unwrap fires `IERC20(asset).transferFrom(VAULT, _to, _amount)`,
    # so VAULT must have an allowance for CT's algod-derived sender.
    set_allowance(usdc_stateful, vault32, ct32, amount)

    # Unwrap leg: alice approves Offramp for her pUSD.
    # Approve Offramp by its algod-derived address — that's what CT
    # sees as `Txn.Sender` when Offramp later calls CT.transferFrom.
    call(collateral_token_wired, "approve",
         [algod_addr_bytes_for_app(collateral_offramp_wired.app_id), amount],
         sender=funded_account)
    call(collateral_offramp_wired, "unwrap",
         [app_id_to_address(usdc_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    assert usdc_balance(usdc_stateful, alice32) == amount
    assert usdc_balance(usdc_stateful, vault32) == 0
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == 0


def test_CollateralOfframp_unwrapUSDCe(
    collateral_onramp_wired, collateral_offramp_wired,
    collateral_token_wired, usdce_stateful, vault, funded_account
):
    """Same shape as unwrapUSDC, USDCe asset slot."""
    from algosdk.encoding import decode_address
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    alice32 = decode_address(funded_account.address)
    vault32 = decode_address(vault.address)
    ct32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    amount = 100_000_000

    deal_usdc(usdce_stateful, alice32, amount)
    set_allowance(usdce_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)
    call(collateral_onramp_wired, "wrap",
         [app_id_to_address(usdce_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    set_allowance(usdce_stateful, vault32, ct32, amount)
    # Approve Offramp by its algod-derived address — that's what CT
    # sees as `Txn.Sender` when Offramp later calls CT.transferFrom.
    call(collateral_token_wired, "approve",
         [algod_addr_bytes_for_app(collateral_offramp_wired.app_id), amount],
         sender=funded_account)
    call(collateral_offramp_wired, "unwrap",
         [app_id_to_address(usdce_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    assert usdc_balance(usdce_stateful, alice32) == amount
    assert usdc_balance(usdce_stateful, vault32) == 0
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == 0


# ── UNWRAP (revert paths) ────────────────────────────────────────────────


def test_revert_CollateralOfframp_unwrapUSDC_paused(
    collateral_offramp, mock_token, admin, funded_account
):
    """When the asset is paused on the Offramp, unwrap reverts with
    OnlyUnpaused before touching the SafeTransferLib path. The Foundry
    test wraps first then pauses then unwraps; here we skip the wrap leg
    since the unwrap revert is independent of any prior wrap state — the
    pause check fires before any state read."""
    call(collateral_offramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            collateral_offramp, "unwrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
            ],
            sender=funded_account,
        )


def test_revert_CollateralOfframp_unwrapUSDCe_paused(
    collateral_offramp, mock_token, admin, funded_account
):
    """Mirror of unwrapUSDC_paused for the USDCe asset slot. On AVM the
    pause table is a single mapping(address ⇒ bool), so any paused asset
    reverts identically; translated 1:1 for parity with the upstream
    count."""
    call(collateral_offramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            collateral_offramp, "unwrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
            ],
            sender=funded_account,
        )


# ── Pausable unpause — positive flow, gated on the same SafeTransferLib gap ──


def test_Pausable_unpause(
    collateral_onramp_wired, collateral_offramp_wired,
    collateral_token_wired, usdc_stateful, vault, admin, funded_account
):
    """Wrap → pause → unwrap reverts → unpause → unwrap succeeds."""
    from algosdk.encoding import decode_address
    from dev.deals import deal_usdc, set_allowance, usdc_balance
    alice32 = decode_address(funded_account.address)
    vault32 = decode_address(vault.address)
    ct32 = algod_addr_bytes_for_app(collateral_token_wired.app_id)
    amount = 100_000_000

    deal_usdc(usdc_stateful, alice32, amount)
    set_allowance(usdc_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)
    call(collateral_onramp_wired, "wrap",
         [app_id_to_address(usdc_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    call(collateral_offramp_wired, "pause",
         [app_id_to_address(usdc_stateful.app_id)], sender=admin)

    set_allowance(usdc_stateful, vault32, ct32, amount)
    # Approve Offramp by its algod-derived address — that's what CT
    # sees as `Txn.Sender` when Offramp later calls CT.transferFrom.
    call(collateral_token_wired, "approve",
         [algod_addr_bytes_for_app(collateral_offramp_wired.app_id), amount],
         sender=funded_account)
    with pytest.raises(LogicError):
        call(collateral_offramp_wired, "unwrap",
             [app_id_to_address(usdc_stateful.app_id),
              addr(funded_account), amount],
             sender=funded_account)

    call(collateral_offramp_wired, "unpause",
         [app_id_to_address(usdc_stateful.app_id)], sender=admin)

    call(collateral_offramp_wired, "unwrap",
         [app_id_to_address(usdc_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)

    assert usdc_balance(usdc_stateful, alice32) == amount
    assert usdc_balance(usdc_stateful, vault32) == 0
    assert call(collateral_token_wired, "balanceOf",
                [addr(funded_account)]) == 0


# Note: test_revert_Pausable_pause_unauthorized lives in
# test_pausable_unauthorized.py (already translated).
