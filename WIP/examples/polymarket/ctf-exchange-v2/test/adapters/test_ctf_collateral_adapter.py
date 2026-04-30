"""Translation of v2 src/test/CtfCollateralAdapter.t.sol — 7 tests
(the 8th, pause_unauthorized, lives in test_adapter_unauthorized.py).

CtfCollateralAdapter.{splitPosition,mergePositions,redeemPositions}
bridges CollateralToken ↔ ConditionalTokens. Each call:

  1. (split) Pulls pUSD from msg.sender via CT.transferFrom, then
     CT.unwrap(USDCE, this, ...) to get USDCE, then CTF.splitPosition,
     then CTF.safeBatchTransferFrom(this, msg.sender, [yes_id, no_id]).
  2. (merge) Reverse of split — CTF.safeBatchTransferFrom(msg.sender,
     this), CTF.mergePositions, USDCE.transfer(CT, ...), CT.wrap(USDCE,
     msg.sender, ...).
  3. (redeem) After payouts reported, CTF.redeemPositions, then re-wrap.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.encoding import decode_address

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.deals import deal_usdc, prepare_condition, set_allowance, usdc_balance
from dev.invoke import call


CONDITION_ID = b"\xc0" * 32


def _canonical_position_ids(helper1, asset_app_id):
    """Compute YES/NO position IDs the adapter will produce for
    `(asset, CONDITION_ID)`. The adapter calls its own
    CallContextChecker__Helper1 internally; helper1 (the Exchange's
    helper) ships the same CTHelpers library so calling it directly
    matches what the adapter computes."""
    asset_addr32 = app_id_to_address(asset_app_id)

    def get_pid(index_set):
        coll_id = helper1.send.call(
            au.AppClientMethodCallParams(
                method="CTHelpers.getCollectionId",
                args=[list(b"\x00" * 32), list(CONDITION_ID), index_set],
                extra_fee=au.AlgoAmount(micro_algo=500_000),
            ),
            send_params=au.SendParams(populate_app_call_resources=True),
        ).abi_return
        coll_id_bytes = bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id
        pid = helper1.send.call(
            au.AppClientMethodCallParams(
                method="CTHelpers.getPositionId",
                args=[bytes(asset_addr32), list(coll_id_bytes)],
                extra_fee=au.AlgoAmount(micro_algo=500_000),
            ),
            send_params=au.SendParams(populate_app_call_resources=True),
        ).abi_return
        return int(pid)

    return get_pid(1), get_pid(2)


def _do_split(
    ctf_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1, *, amount=100_000_000,
):
    """Common setup: fund alice with `amount` pUSD (via Onramp.wrap), prepare
    the CTF condition with canonical YES/NO ids, then call adapter.splitPosition.
    Returns (alice32, yes_id, no_id, amount)."""
    alice32 = decode_address(funded_account.address)
    yes_id, no_id = _canonical_position_ids(helper1, usdce_stateful.app_id)
    prepare_condition(ctf_stateful, CONDITION_ID, yes_id, no_id)

    deal_usdc(usdce_stateful, alice32, amount)
    set_allowance(usdce_stateful, alice32,
                  algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                  amount)
    call(collateral_onramp_wired, "wrap",
         [app_id_to_address(usdce_stateful.app_id),
          addr(funded_account), amount],
         sender=funded_account)
    set_allowance(usdce_stateful, decode_address(vault.address),
                  algod_addr_bytes_for_app(collateral_token_wired.app_id),
                  amount)
    call(collateral_token_wired, "approve",
         [algod_addr_bytes_for_app(ctf_adapter_wired.app_id), amount],
         sender=funded_account)

    composer = _ctf_heavy_group(
        ctf_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id, usdce_stateful.app_id,
        main_method="splitPosition",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))
    return alice32, yes_id, no_id, amount


def _ctf_heavy_group(client, sender_account, ct_id, ctf_id, usdce_id,
                     *, main_method, main_args):
    """Build a group for a CTF adapter heavy main call (split/merge/redeem).

    Each pad pre-pins one foreign app via `app_references` so algokit's
    auto-populate has a per-app "carrier" txn for that app's box refs.
    Without pre-pinning, auto-populate piles all 3 apps + their boxes
    onto one pad (~9 refs) and exceeds MaxAppTotalTxnReferences=8 — flaky
    depending on simulate's enumeration order.
    """
    composer = client.algorand.new_group()
    for i, app_id in enumerate([usdce_id, ct_id, ctf_id]):
        composer = composer.add_app_call_method_call(
            client.params.call(au.AppClientMethodCallParams(
                method="paused", args=[bytes([i + 5]) * 32],
                sender=sender_account.address,
                note=f"pad-{i+1}".encode(),
                app_references=[app_id])))
    composer = composer.add_app_call_method_call(
        client.params.call(au.AppClientMethodCallParams(
            method=main_method, args=main_args,
            sender=sender_account.address,
            extra_fee=au.AlgoAmount(micro_algo=300_000),
            app_references=[ct_id, ctf_id, usdce_id])))
    return composer


def test_CtfCollateralAdapter_splitPosition(
    ctf_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1
):
    """alice splits pUSD into YES + NO via the adapter."""
    alice32, yes_id, no_id, amount = _do_split(
        ctf_adapter_wired, collateral_token_wired, ctf_stateful,
        usdce_stateful, collateral_onramp_wired, vault, funded_account, helper1)

    # alice has YES + NO, no pUSD.
    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == amount
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == amount
    assert call(collateral_token_wired, "balanceOf", [alice32]) == 0


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
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2], 100_000_000],
            sender=funded_account,
        )


def test_revert_CtfCollateralAdapter_mergePositions_paused(
    ctf_adapter, mock_token, admin, funded_account
):
    call(ctf_adapter, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "mergePositions",
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2], 100_000_000],
            sender=funded_account,
        )


def test_revert_CtfCollateralAdapter_redeemPositions_paused(
    ctf_adapter, mock_token, admin, funded_account
):
    call(ctf_adapter, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            ctf_adapter, "redeemPositions",
            [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2]],
            sender=funded_account,
        )


def test_CtfCollateralAdapter_mergePositions(
    ctf_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1
):
    """alice splits to YES+NO, then immediately merges back to pUSD."""
    from dev.deals import set_approval
    alice32, yes_id, no_id, amount = _do_split(
        ctf_adapter_wired, collateral_token_wired, ctf_stateful,
        usdce_stateful, collateral_onramp_wired, vault, funded_account, helper1)

    # alice now has YES + NO. She approves the adapter to pull them.
    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(ctf_adapter_wired.app_id), True)

    composer = _ctf_heavy_group(
        ctf_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id, usdce_stateful.app_id,
        main_method="mergePositions",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))

    # YES+NO burned, alice gets pUSD back.
    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == 0
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


def test_CtfCollateralAdapter_unpause(
    ctf_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1, admin
):
    """Pause USDCE → splitPosition reverts. Unpause → splitPosition succeeds."""
    # Pause USDCE on the adapter.
    call(ctf_adapter_wired, "pause",
         [app_id_to_address(usdce_stateful.app_id)], sender=admin)

    # First splitPosition attempt should revert.
    alice32 = decode_address(funded_account.address)
    yes_id, no_id = _canonical_position_ids(helper1, usdce_stateful.app_id)
    prepare_condition(ctf_stateful, CONDITION_ID, yes_id, no_id)
    amount = 100_000_000

    with pytest.raises(LogicError):
        call(ctf_adapter_wired, "splitPosition",
             [b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
             sender=funded_account)

    # Unpause and run the full split flow.
    call(ctf_adapter_wired, "unpause",
         [app_id_to_address(usdce_stateful.app_id)], sender=admin)

    alice32, _yes, _no, amount = _do_split(
        ctf_adapter_wired, collateral_token_wired, ctf_stateful,
        usdce_stateful, collateral_onramp_wired, vault, funded_account, helper1)
    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == amount
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == amount


# ── redeemPositions ─────────────────────────────────────────────────────


def _report_payouts(ctf, condition_id, yes_payout, no_payout):
    return ctf.send.call(au.AppClientMethodCallParams(
        method="report_payouts",
        args=[list(condition_id), yes_payout, no_payout],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
        box_references=[au.BoxReference(app_id=0, name=b"po_" + bytes(condition_id))],
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return


@pytest.mark.parametrize("outcome", [True, False], ids=["yes", "no"])
def test_CtfCollateralAdapter_redeemPositions(
    ctf_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1, outcome
):
    """alice splits, oracle reports payouts, alice redeems → gets pUSD back.

    With binary outcome: payouts = (1, 0) for YES, (0, 1) for NO.
    Either way alice still has full pUSD value (she has both YES and NO,
    so combined payout = amount * 1 = amount)."""
    from dev.deals import set_approval
    alice32, yes_id, no_id, amount = _do_split(
        ctf_adapter_wired, collateral_token_wired, ctf_stateful,
        usdce_stateful, collateral_onramp_wired, vault, funded_account, helper1)

    # Oracle reports.
    yes_payout, no_payout = (1, 0) if outcome else (0, 1)
    _report_payouts(ctf_stateful, CONDITION_ID, yes_payout, no_payout)

    # alice approves the adapter to pull her positions.
    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(ctf_adapter_wired.app_id), True)

    composer = _ctf_heavy_group(
        ctf_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id, usdce_stateful.app_id,
        main_method="redeemPositions",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2]],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))

    # alice's CTF balances zero, pUSD restored.
    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == 0
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


# Note: test_revert_CtfCollateralAdapter_pause_unauthorized lives in
# test_adapter_unauthorized.py (already translated).
