"""Translation of v2 src/test/NegRiskCtfCollateralAdapter.t.sol — 12 tests.

NegRiskCtfCollateralAdapter inherits CtfCollateralAdapter — the
split/merge/redeem/pause flows are the same shape. The NegRisk-specific
`convertPositions` flow + 5 fuzz cases (one/two/three NO→YES, with fees,
zero amount) need a real NegRiskAdapter mock with `getQuestionCount`,
`getFeeBips`, `convertPositions`, `getPositionId` — those stay xfailed
under one umbrella reason.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.encoding import decode_address

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.deals import deal_usdc, prepare_condition, set_allowance
from dev.invoke import call


CONDITION_ID = b"\xc0" * 32


def _canonical_position_ids(helper1, asset_app_id):
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
    negrisk_adapter_wired, collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1, *, amount=100_000_000,
):
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
         [algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), amount],
         sender=funded_account)

    composer = (
        negrisk_adapter_wired.algorand.new_group()
        .add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method="USDCE", args=[],
                sender=funded_account.address, note=b"pad-1")))
        .add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method="COLLATERAL_TOKEN", args=[],
                sender=funded_account.address, note=b"pad-2")))
        .add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method="CONDITIONAL_TOKENS", args=[],
                sender=funded_account.address, note=b"pad-3")))
        .add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method="USDCE", args=[],
                sender=funded_account.address, note=b"pad-4")))
        .add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method="splitPosition",
                args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
                sender=funded_account.address,
                extra_fee=au.AlgoAmount(micro_algo=300_000),
                app_references=[
                    collateral_token_wired.app_id,
                    ctf_stateful.app_id,
                    usdce_stateful.app_id,
                ])))
    )
    composer.send(au.SendParams(populate_app_call_resources=True))
    return alice32, yes_id, no_id, amount


# NegRiskCtfCollateralAdapter overrides _splitPosition / _mergePositions /
# _redeemPositions to delegate into INegRiskAdapter(NEG_RISK_ADAPTER).
# Tests use a stateful NegRiskAdapter mock (delegate/negrisk_adapter_mock.py)
# that mints/burns position tokens on CTFMock under the WRAPPED_COLLATERAL
# position IDs (= IDs derived from the NegRiskAdapter mock's own address).


def _pad_group(client, sender_account, n_pads):
    """Build a group with `n_pads` no-op global-state-read calls — gives
    the simulator's auto-populate enough foreign-app/box reference budget
    when chained to a heavy main call."""
    methods = ["USDCE", "COLLATERAL_TOKEN", "CONDITIONAL_TOKENS"]
    composer = client.algorand.new_group()
    for i in range(n_pads):
        m = methods[i % len(methods)]
        composer = composer.add_app_call_method_call(
            client.params.call(au.AppClientMethodCallParams(
                method=m, args=[],
                sender=sender_account.address,
                note=f"pad-{i}".encode())))
    return composer


def _negrisk_canonical_position_ids(helper1, wcol_addr32):
    """Same as `_canonical_position_ids` but using `wcol_addr32` (= the
    NegRiskAdapter mock's address) as the collateral input."""
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
                args=[bytes(wcol_addr32), list(coll_id_bytes)],
                extra_fee=au.AlgoAmount(micro_algo=500_000),
            ),
            send_params=au.SendParams(populate_app_call_resources=True),
        ).abi_return
        return int(pid)

    return get_pid(1), get_pid(2)


def _do_negrisk_split(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
    *, amount=100_000_000,
):
    """alice wraps USDCe → pUSD → adapter.splitPosition.

    The adapter's _getPositionIds uses WRAPPED_COLLATERAL = wcol() =
    NegRiskAdapter mock's algod-derived address. Pre-compute the
    canonical position IDs from that, register them on the NegRiskAdapter
    mock so its splitPosition delegate knows which CTFMock token IDs to
    mint."""
    alice32 = decode_address(funded_account.address)
    wcol_addr32 = algod_addr_bytes_for_app(negrisk_adapter_mock.app_id)
    yes_id, no_id = _negrisk_canonical_position_ids(helper1, wcol_addr32)

    # Register partition on the NegRiskAdapter mock (CTFMock isn't used
    # for splitPosition lookup in the negrisk variant — the mock's own
    # delegate handles minting).
    negrisk_adapter_mock.send.call(au.AppClientMethodCallParams(
        method="prepare_condition",
        args=[list(CONDITION_ID), yes_id, no_id],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=au.SendParams(populate_app_call_resources=True))

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
         [algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), amount],
         sender=funded_account)

    composer = _negrisk_heavy_group(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id,
        usdce_stateful.app_id, negrisk_adapter_mock.app_id,
        main_method="splitPosition",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))
    return alice32, yes_id, no_id, amount


def _negrisk_heavy_group(
    client, sender_account, ct_id, ctf_id, usdce_id, nrm_id,
    *, main_method, main_args,
):
    """Build a group for a NegRisk heavy main call (split/merge/redeem).

    Each pad pins a single foreign app via `app_references` so algokit's
    auto-populate has a pre-existing "carrier" txn for each app's boxes.
    Without this, auto-populate piles all 4 apps + their boxes onto the
    first pad and exceeds MaxAppTotalTxnReferences=8.

    Why this works: `populate_group_resource` adds box refs to whichever
    txn already references the box's app (lenient < 8 check). By pinning
    one app per pad, USDCe boxes go to pad-USDCe, CT boxes to pad-CT, etc.
    The main call carries all 4 apps; auto-populate fills its remaining
    slots with the few main-only resources.
    """
    composer = client.algorand.new_group()
    # Each pad reads global state only, but pre-references one foreign
    # app so auto-populate routes that app's boxes here.
    for i, app_id in enumerate([usdce_id, ct_id, ctf_id, nrm_id]):
        composer = composer.add_app_call_method_call(
            client.params.call(au.AppClientMethodCallParams(
                method="USDCE", args=[],
                sender=sender_account.address,
                note=f"pad-{i}".encode(),
                app_references=[app_id])))
    composer = composer.add_app_call_method_call(
        client.params.call(au.AppClientMethodCallParams(
            method=main_method,
            args=main_args,
            sender=sender_account.address,
            extra_fee=au.AlgoAmount(micro_algo=400_000),
            app_references=[ct_id, ctf_id, usdce_id, nrm_id])))
    return composer


def test_NegRiskCtfCollateralAdapter_splitPosition(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1
):
    alice32, yes_id, no_id, amount = _do_negrisk_split(
        negrisk_adapter_wired, negrisk_adapter_mock,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        collateral_onramp_wired, vault, funded_account, helper1)
    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == amount
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == amount
    assert call(collateral_token_wired, "balanceOf", [alice32]) == 0


def test_NegRiskCtfCollateralAdapter_mergePositions(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1
):
    """alice splits to YES+NO via NegRisk adapter, then merges back to pUSD."""
    from dev.deals import set_approval
    alice32, yes_id, no_id, amount = _do_negrisk_split(
        negrisk_adapter_wired, negrisk_adapter_mock,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        collateral_onramp_wired, vault, funded_account, helper1)

    # alice approves the wired adapter to pull her YES+NO via the outer
    # CTF.safeBatchTransferFrom in mergePositions.
    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), True)

    composer = _negrisk_heavy_group(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id,
        usdce_stateful.app_id, negrisk_adapter_mock.app_id,
        main_method="mergePositions",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))

    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == 0
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


@pytest.mark.parametrize("outcome", [True, False], ids=["yes", "no"])
def test_NegRiskCtfCollateralAdapter_redeemPositions(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1, outcome
):
    """alice splits, NegRisk reports payouts, alice redeems → gets pUSD back."""
    from dev.deals import set_approval
    alice32, yes_id, no_id, amount = _do_negrisk_split(
        negrisk_adapter_wired, negrisk_adapter_mock,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        collateral_onramp_wired, vault, funded_account, helper1)

    yes_payout, no_payout = (1, 0) if outcome else (0, 1)
    negrisk_adapter_mock.send.call(au.AppClientMethodCallParams(
        method="report_payouts",
        args=[list(CONDITION_ID), yes_payout, no_payout],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=au.SendParams(populate_app_call_resources=True))

    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), True)

    composer = _negrisk_heavy_group(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id,
        usdce_stateful.app_id, negrisk_adapter_mock.app_id,
        main_method="redeemPositions",
        main_args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2]],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))

    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == 0
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


# ── convertPositions: NegRisk-specific, multi-question setup ──


# marketId with low byte = 0 so marketId | questionIdx gives a clean
# questionId. Matches the EVM convention `bytes32(uint256(marketId) | i)`.
# Low byte = 0 so MARKET_ID | i gives clean questionId. Base byte 0x42
# lands all 4 conditionIds (0x42*31 + 0x00..0x03) on EC-sqrt fast paths
# in CTHelpers.getCollectionId; 0xc0 hits a slow path on q3 (~120k ops).
MARKET_ID = b"\x42" * 31 + b"\x00"


def _canonical_position_ids_for(helper1, wcol_addr32, condition_id):
    """Return (yes_id, no_id) for `condition_id` derived from `wcol_addr32`."""
    def _pid(index_set):
        coll_id = helper1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getCollectionId",
            args=[list(b"\x00" * 32), list(condition_id), index_set],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        coll_id_bytes = bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id
        pid = helper1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getPositionId",
            args=[bytes(wcol_addr32), list(coll_id_bytes)],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        return int(pid)
    return _pid(1), _pid(2)


def _convert_setup(
    *, negrisk_adapter_wired, negrisk_adapter_mock, collateral_token_wired,
    ctf_stateful, usdce_stateful, collateral_onramp_wired, vault,
    funded_account, helper1, question_count, fee_bips=0,
    amount=100_000_000,
):
    """Prepare a NegRisk market with `question_count` questions, register
    canonical YES/NO IDs on the mock, fund alice with N*amount pUSD, split
    each question so alice has YES + NO for each.

    Returns (alice32, market_id, [question_ids], [(yes_id, no_id), ...])."""
    from dev.deals import deal_usdc, set_allowance, set_approval

    alice32 = decode_address(funded_account.address)
    wcol_addr32 = algod_addr_bytes_for_app(negrisk_adapter_mock.app_id)

    # Register market.
    negrisk_adapter_mock.send.call(au.AppClientMethodCallParams(
        method="prepare_market",
        args=[list(MARKET_ID), question_count, fee_bips],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=au.SendParams(populate_app_call_resources=True))

    # For each question: register canonical YES/NO IDs in CTFMock, mock's
    # partition (for splitPosition) AND mock's question table (for
    # convertPositions). We use questionId == conditionId for test
    # simplicity — CTHelpers.getCollectionId only depends on the
    # conditionId bytes.
    question_ids = []
    pos_ids = []
    for i in range(question_count):
        qid = MARKET_ID[:31] + bytes([i])
        yes_id, no_id = _canonical_position_ids_for(helper1, wcol_addr32, qid)
        prepare_condition(ctf_stateful, qid, yes_id, no_id)
        negrisk_adapter_mock.send.call(au.AppClientMethodCallParams(
            method="prepare_condition",
            args=[list(qid), yes_id, no_id],
            extra_fee=au.AlgoAmount(micro_algo=10_000),
        ), send_params=au.SendParams(populate_app_call_resources=True))
        negrisk_adapter_mock.send.call(au.AppClientMethodCallParams(
            method="prepare_question",
            args=[list(qid), yes_id, no_id],
            extra_fee=au.AlgoAmount(micro_algo=10_000),
        ), send_params=au.SendParams(populate_app_call_resources=True))
        question_ids.append(qid)
        pos_ids.append((yes_id, no_id))

    # Fund alice with N*amount pUSD via Onramp.wrap.
    total = amount * question_count
    if total > 0:
        deal_usdc(usdce_stateful, alice32, total)
        set_allowance(usdce_stateful, alice32,
                      algod_addr_bytes_for_app(collateral_onramp_wired.app_id),
                      total)
        call(collateral_onramp_wired, "wrap",
             [app_id_to_address(usdce_stateful.app_id),
              addr(funded_account), total],
             sender=funded_account)
        set_allowance(usdce_stateful, decode_address(vault.address),
                      algod_addr_bytes_for_app(collateral_token_wired.app_id),
                      total)
        call(collateral_token_wired, "approve",
             [algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), total],
             sender=funded_account)

        # Split each question — alice ends with `amount` YES + `amount` NO
        # for each question.
        for i in range(question_count):
            composer = _negrisk_heavy_group(
                negrisk_adapter_wired, funded_account,
                collateral_token_wired.app_id, ctf_stateful.app_id,
                usdce_stateful.app_id, negrisk_adapter_mock.app_id,
                main_method="splitPosition",
                main_args=[b"\x00" * 32, b"\x00" * 32, question_ids[i],
                           [1, 2], amount],
            )
            composer.send(au.SendParams(populate_app_call_resources=True))

    # alice approves the wired adapter to pull her NO tokens for convert.
    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), True)
    return alice32, MARKET_ID, question_ids, pos_ids


def _do_convert(
    negrisk_adapter_wired, funded_account,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    negrisk_adapter_mock,
    *, market_id, index_set, amount,
):
    """Run the wired adapter's convertPositions in a heavy-pad group.
    convertPositions chains: pull NO from alice → mock.convertPositions
    (burns NO + mints YES + sends USDCe to wired adapter) → wired sends
    YES to alice + wraps USDCe to pUSD."""
    composer = _negrisk_heavy_group(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired.app_id, ctf_stateful.app_id,
        usdce_stateful.app_id, negrisk_adapter_mock.app_id,
        main_method="convertPositions",
        main_args=[market_id, index_set, amount],
    )
    composer.send(au.SendParams(populate_app_call_resources=True))


def test_NegRiskCtfCollateralAdapter_convertPositions_oneNoToYes(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
):
    """indexSet = 0b0001: pull NO_0, mint YES_{1,2,3}. No collateral."""
    amount = 100_000_000
    alice32, market_id, qids, pos = _convert_setup(
        negrisk_adapter_wired=negrisk_adapter_wired,
        negrisk_adapter_mock=negrisk_adapter_mock,
        collateral_token_wired=collateral_token_wired,
        ctf_stateful=ctf_stateful, usdce_stateful=usdce_stateful,
        collateral_onramp_wired=collateral_onramp_wired,
        vault=vault, funded_account=funded_account, helper1=helper1,
        question_count=4, amount=amount,
    )
    _do_convert(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        negrisk_adapter_mock,
        market_id=market_id, index_set=1, amount=amount,
    )
    # NO_0 spent.
    assert call(ctf_stateful, "balanceOf", [alice32, pos[0][1]]) == 0
    # YES_{1,2,3}: split (amount) + convert (amount) = 2*amount.
    for i in range(1, 4):
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][0]]) == 2 * amount
    # No collateral returned (pullCount=1 → 0 USDCe).
    assert call(collateral_token_wired, "balanceOf", [alice32]) == 0


def test_NegRiskCtfCollateralAdapter_convertPositions_twoNoToYes(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
):
    """indexSet = 0b0011: pull NO_{0,1}, mint YES_{2,3}, return amount pUSD."""
    amount = 100_000_000
    alice32, market_id, qids, pos = _convert_setup(
        negrisk_adapter_wired=negrisk_adapter_wired,
        negrisk_adapter_mock=negrisk_adapter_mock,
        collateral_token_wired=collateral_token_wired,
        ctf_stateful=ctf_stateful, usdce_stateful=usdce_stateful,
        collateral_onramp_wired=collateral_onramp_wired,
        vault=vault, funded_account=funded_account, helper1=helper1,
        question_count=4, amount=amount,
    )
    _do_convert(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        negrisk_adapter_mock,
        market_id=market_id, index_set=3, amount=amount,
    )
    for i in range(2):
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][1]]) == 0
    for i in range(2, 4):
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][0]]) == 2 * amount
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


def test_NegRiskCtfCollateralAdapter_convertPositions_threeNoToYes(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
):
    """indexSet = 0b0111: pull NO_{0,1,2}, mint YES_3, return 2*amount pUSD."""
    amount = 100_000_000
    alice32, market_id, qids, pos = _convert_setup(
        negrisk_adapter_wired=negrisk_adapter_wired,
        negrisk_adapter_mock=negrisk_adapter_mock,
        collateral_token_wired=collateral_token_wired,
        ctf_stateful=ctf_stateful, usdce_stateful=usdce_stateful,
        collateral_onramp_wired=collateral_onramp_wired,
        vault=vault, funded_account=funded_account, helper1=helper1,
        question_count=4, amount=amount,
    )
    _do_convert(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        negrisk_adapter_mock,
        market_id=market_id, index_set=7, amount=amount,
    )
    for i in range(3):
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][1]]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, pos[3][0]]) == 2 * amount
    assert call(collateral_token_wired, "balanceOf", [alice32]) == 2 * amount


def test_NegRiskCtfCollateralAdapter_convertPositions_withFees(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
):
    """200 bips fee on indexSet=0b0011, amount=100M.
    amountOut = 100M - 100M*200/10000 = 98M.
    YES_{2,3} = 100M (split) + 98M (convert) = 198M.
    Collateral = (2-1) * 98M = 98M."""
    amount = 100_000_000
    alice32, market_id, qids, pos = _convert_setup(
        negrisk_adapter_wired=negrisk_adapter_wired,
        negrisk_adapter_mock=negrisk_adapter_mock,
        collateral_token_wired=collateral_token_wired,
        ctf_stateful=ctf_stateful, usdce_stateful=usdce_stateful,
        collateral_onramp_wired=collateral_onramp_wired,
        vault=vault, funded_account=funded_account, helper1=helper1,
        question_count=4, fee_bips=200, amount=amount,
    )
    _do_convert(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        negrisk_adapter_mock,
        market_id=market_id, index_set=3, amount=amount,
    )
    fee = amount * 200 // 10_000
    amount_out = amount - fee
    for i in range(2, 4):
        assert call(ctf_stateful, "balanceOf",
                    [alice32, pos[i][0]]) == amount + amount_out
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount_out


def test_NegRiskCtfCollateralAdapter_convertPositions_zeroAmount(
    negrisk_adapter_wired, negrisk_adapter_mock,
    collateral_token_wired, ctf_stateful, usdce_stateful,
    collateral_onramp_wired, vault, funded_account, helper1,
):
    """convertPositions(_, _, 0) is a no-op — no balance changes."""
    amount = 100_000_000
    alice32, market_id, qids, pos = _convert_setup(
        negrisk_adapter_wired=negrisk_adapter_wired,
        negrisk_adapter_mock=negrisk_adapter_mock,
        collateral_token_wired=collateral_token_wired,
        ctf_stateful=ctf_stateful, usdce_stateful=usdce_stateful,
        collateral_onramp_wired=collateral_onramp_wired,
        vault=vault, funded_account=funded_account, helper1=helper1,
        question_count=4, amount=amount,
    )
    ct_before = call(collateral_token_wired, "balanceOf", [alice32])
    _do_convert(
        negrisk_adapter_wired, funded_account,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        negrisk_adapter_mock,
        market_id=market_id, index_set=3, amount=0,
    )
    assert call(collateral_token_wired, "balanceOf", [alice32]) == ct_before
    # YES + NO unchanged.
    for i in range(4):
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][0]]) == amount
        assert call(ctf_stateful, "balanceOf", [alice32, pos[i][1]]) == amount


# ── PAUSE-REVERT paths ──────────────────────────────────────────────────


def test_revert_NegRiskCtfCollateralAdapter_splitPosition_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(negrisk_adapter, "splitPosition",
             [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2], 100_000_000],
             sender=funded_account)


def test_revert_NegRiskCtfCollateralAdapter_mergePositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(negrisk_adapter, "mergePositions",
             [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2], 100_000_000],
             sender=funded_account)


def test_revert_NegRiskCtfCollateralAdapter_redeemPositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(negrisk_adapter, "redeemPositions",
             [b"\x00" * 32, b"\x00" * 32, b"\x00" * 32, [1, 2]],
             sender=funded_account)


def test_revert_NegRiskCtfCollateralAdapter_convertPositions_paused(
    negrisk_adapter, universal_mock, admin, funded_account
):
    call(negrisk_adapter, "pause",
         [app_id_to_address(universal_mock.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(negrisk_adapter, "convertPositions",
             [b"\x00" * 32, 1, 100_000_000],
             sender=funded_account)
