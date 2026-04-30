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

    # The split chain in NegRisk is deeper than CtfAdapter (adapter →
    # NegRiskAdapter mock → USDCe + 2× CTFMock.mint), so we add 5 pads
    # to the group to give auto-populate enough ref budget. Pads use
    # CONDITIONAL_TOKENS() / COLLATERAL_TOKEN() / USDCE() global-state
    # reads which carry zero box refs.
    composer = negrisk_adapter_wired.algorand.new_group()
    pad_methods = ["USDCE", "COLLATERAL_TOKEN", "CONDITIONAL_TOKENS",
                   "USDCE", "COLLATERAL_TOKEN"]
    for i, m in enumerate(pad_methods):
        composer = composer.add_app_call_method_call(
            negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
                method=m, args=[],
                sender=funded_account.address, note=f"pad-{i}".encode())))
    composer = composer.add_app_call_method_call(
        negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
            method="splitPosition",
            args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
            sender=funded_account.address,
            extra_fee=au.AlgoAmount(micro_algo=400_000),
            app_references=[
                collateral_token_wired.app_id,
                ctf_stateful.app_id,
                usdce_stateful.app_id,
                negrisk_adapter_mock.app_id,
            ])))
    composer.send(au.SendParams(populate_app_call_resources=True))
    return alice32, yes_id, no_id, amount


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
    from dev.deals import set_approval
    alice32, yes_id, no_id, amount = _do_negrisk_split(
        negrisk_adapter_wired, negrisk_adapter_mock,
        collateral_token_wired, ctf_stateful, usdce_stateful,
        collateral_onramp_wired, vault, funded_account, helper1)

    set_approval(ctf_stateful, alice32,
                 algod_addr_bytes_for_app(negrisk_adapter_wired.app_id), True)

    composer = _pad_group(negrisk_adapter_wired, funded_account, 7)
    composer = composer.add_app_call_method_call(
        negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
            method="mergePositions",
            args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2], amount],
            sender=funded_account.address,
            extra_fee=au.AlgoAmount(micro_algo=400_000))))
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

    composer = _pad_group(negrisk_adapter_wired, funded_account, 7)
    composer = composer.add_app_call_method_call(
        negrisk_adapter_wired.params.call(au.AppClientMethodCallParams(
            method="redeemPositions",
            args=[b"\x00" * 32, b"\x00" * 32, CONDITION_ID, [1, 2]],
            sender=funded_account.address,
            extra_fee=au.AlgoAmount(micro_algo=400_000))))
    composer.send(au.SendParams(populate_app_call_resources=True))

    assert call(ctf_stateful, "balanceOf", [alice32, yes_id]) == 0
    assert call(ctf_stateful, "balanceOf", [alice32, no_id]) == 0
    assert call(collateral_token_wired, "balanceOf", [alice32]) == amount


# ── convertPositions: NegRisk-specific, needs a real NegRiskAdapter mock ──


_NEEDS_NEGRISK_MOCK = (
    "convertPositions calls INegRiskAdapter(NEG_RISK_ADAPTER).{getQuestionCount,"
    "getFeeBips,convertPositions} via inner-tx — these need a stateful NegRisk "
    "adapter mock that the universal_mock doesn't provide."
)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_oneNoToYes(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_twoNoToYes(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_threeNoToYes(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_withFees(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK, strict=True)
def test_NegRiskCtfCollateralAdapter_convertPositions_zeroAmount(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK)


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
