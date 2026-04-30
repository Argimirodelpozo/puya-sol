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
# _redeemPositions to call `INegRiskAdapter(NEG_RISK_ADAPTER).{split,merge,
# redeem}Positions(...)` instead of going through CONDITIONAL_TOKENS
# directly. universal_mock doesn't implement those methods (its match
# list has only ~12 no-op labels), so the inherited test pattern from
# CtfCollateralAdapter doesn't translate — we'd need a stateful
# NegRiskAdapter mock that mints/burns wrapped-collateral position IDs
# the same way the real NegRisk adapter does. Out of scope for now.


_NEEDS_NEGRISK_MOCK_FULL = (
    "NegRiskCtfCollateralAdapter overrides _splitPosition/_mergePositions/"
    "_redeemPositions to delegate into INegRiskAdapter(NEG_RISK_ADAPTER), "
    "which universal_mock doesn't implement. A real NegRiskAdapter mock "
    "(state for wrapped-collateral position IDs + matching split/merge/"
    "redeem methods) is needed; tracked separately."
)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK_FULL, strict=True)
def test_NegRiskCtfCollateralAdapter_splitPosition(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK_FULL)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK_FULL, strict=True)
def test_NegRiskCtfCollateralAdapter_mergePositions(negrisk_adapter):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK_FULL)


@pytest.mark.xfail(reason=_NEEDS_NEGRISK_MOCK_FULL, strict=True)
@pytest.mark.parametrize("outcome", [True, False], ids=["yes", "no"])
def test_NegRiskCtfCollateralAdapter_redeemPositions(negrisk_adapter, outcome):
    raise NotImplementedError(_NEEDS_NEGRISK_MOCK_FULL)


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
