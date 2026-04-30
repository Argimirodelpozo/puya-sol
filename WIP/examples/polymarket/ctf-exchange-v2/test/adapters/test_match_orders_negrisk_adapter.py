"""Translation of v2 src/test/MatchOrdersNegRiskCtfCollateralAdapter.t.sol —
8 tests.

Mirror of MatchOrdersCtfCollateralAdapter, but with NegRiskCtfCollateralAdapter.

Differences vs the plain CTF adapter variant:
  - `outcomeTokenFactory` = NegRiskCtfCollateralAdapter (delegates split/merge
    into the stateful NegRiskAdapter mock instead of CTFMock directly).
  - `ctfCollateral` = the NegRiskAdapter mock's ALGOD-form address (= wcol()).
    The plain CTF adapter case has both orch.ctfCollateral and adapter.USDCE
    in the same puya-sol-conv form so they collapse — for NegRisk the
    adapter's `WRAPPED_COLLATERAL = INegRiskAdapter(_negRiskAdapter).wcol()`
    is computed at construction and resolves to algod-form. matchOrders'
    token-id validation hashes orch.ctfCollateral; the adapter's
    `_getPositionIds` hashes WRAPPED_COLLATERAL — the two must use the same
    bytes. The `split_negrisk_adapter_with_delegate` fixture overrides
    orch's stored ctfCollateral to algod-form to match.
  - The NegRiskAdapter mock has its own partition store (BoxMap `p_<condId>`)
    so each test calls `prepare_condition` on it.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.encoding import decode_address
from hashlib import sha256

from dev.addrs import addr, algod_addr_bytes_for_app, app_id_to_address
from dev.deals import (
    deal_usdc as deal_usdce, set_allowance as set_usdce_allowance,
    deal_outcome, set_approval, prepare_condition,
)
from dev.deploy import deploy_app
from dev.invoke import call
from dev.match_dispatch import dance_match_orders
from dev.orders import make_order, sign_order, Side, SignatureType
from dev.signing import bob, carla


CONDITION_ID = b"\xc0" * 32


def _deploy_maker_wallet(localnet, admin, *, owner_eth_addr32: bytes):
    from conftest import OUT_DIR
    base = OUT_DIR / "test" / "dev" / "mocks" / "MakerWallet1271"
    return deploy_app(
        localnet, admin, base, "MakerWallet1271",
        create_args=[bytes(owner_eth_addr32),
                     bytes(decode_address(admin.address))],
    )


def _negrisk_canonical_yes_no_ids(h1, negrisk_mock_app_id):
    """Position ids via wcol() = NRM mock's algod-form. Matches what the
    adapter's `_getPositionIds(condId) → CTFHelpers.positionIds(WRAPPED_
    COLLATERAL, condId)` produces, and what the orch's ctfCollateral
    points to in the NegRisk fixture."""
    coll_addr32 = bytes(algod_addr_bytes_for_app(negrisk_mock_app_id))

    def _pid(index_set):
        coll_id = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getCollectionId",
            args=[list(b"\x00" * 32), list(CONDITION_ID), index_set],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        coll_id_bytes = bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id
        pid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getPositionId",
            args=[coll_addr32, list(coll_id_bytes)],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        return int(pid)

    return _pid(1), _pid(2)


def _fund_wallet_pusd(
    *, wallet_psol, ct, usdce, onramp, vault, h1_app_id, admin, amount,
):
    admin32 = decode_address(admin.address)
    deal_usdce(usdce, admin32, amount)
    set_usdce_allowance(usdce, admin32,
                        algod_addr_bytes_for_app(onramp.app_id), amount)
    call(onramp, "wrap",
         [app_id_to_address(usdce.app_id), wallet_psol, amount],
         sender=admin)
    set_usdce_allowance(usdce, decode_address(vault.address),
                        algod_addr_bytes_for_app(ct.app_id), amount)
    call(ct, "avmPortForceApprove",
         [wallet_psol, algod_addr_bytes_for_app(h1_app_id), amount],
         sender=admin)


def _fund_wallet_outcome(ctf, wallet_psol, h1_addr, token_id, amount):
    deal_outcome(ctf, wallet_psol, token_id, amount)
    set_approval(ctf, wallet_psol, h1_addr, True)


def _setup_negrisk_partition(negrisk_mock, yes_id, no_id):
    """Register the YES/NO partition on the NegRiskAdapter mock so its
    splitPosition/mergePositions delegate knows which CTFMock token ids
    to mint/burn."""
    negrisk_mock.send.call(au.AppClientMethodCallParams(
        method="prepare_condition",
        args=[list(CONDITION_ID), yes_id, no_id],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=au.SendParams(populate_app_call_resources=True))


def _wallets_and_ids(localnet, admin, h1, negrisk_mock):
    bob_signer, carla_signer = bob(), carla()
    bob_wallet = _deploy_maker_wallet(
        localnet, admin, owner_eth_addr32=bob_signer.eth_address_padded32)
    carla_wallet = _deploy_maker_wallet(
        localnet, admin, owner_eth_addr32=carla_signer.eth_address_padded32)
    bob_psol = bytes(app_id_to_address(bob_wallet.app_id))
    carla_psol = bytes(app_id_to_address(carla_wallet.app_id))
    yes_id, no_id = _negrisk_canonical_yes_no_ids(h1, negrisk_mock.app_id)
    return (bob_signer, carla_signer, bob_wallet, carla_wallet,
            bob_psol, carla_psol, yes_id, no_id)


# ── happy paths ────────────────────────────────────────────────────────


def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint(
    split_negrisk_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Both BUYs of complementary tokens via NegRisk adapter.
    matchOrders pulls pUSD from both wallets, calls
    NegRiskCtfCollateralAdapter.splitPosition which unwraps pUSD →
    USDCe → forwards to the NegRiskAdapter mock for token mint, then
    distributes YES/NO from adapter to wallets."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    _fund_wallet_pusd(
        wallet_psol=carla_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    set_usdce_allowance(
        usdce, decode_address(vault.address),
        algod_addr_bytes_for_app(ct.app_id), 1_000_000_000,
    )

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    nrm_addr = algod_addr_bytes_for_app(negrisk_mock.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        # CT (pUSD) — collect from wallets, distribute is via adapter
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(orch_addr + adapter_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + orch_addr),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + ct_addr),
        # USDCe — VAULT → adapter (CT.unwrap) → NRM (NRM.splitPosition pulls)
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + decode_address(vault.address)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + adapter_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + nrm_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + ct_addr),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"a_" + sha256(adapter_addr + nrm_addr).digest()),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"a_" + sha256(ct_addr + adapter_addr).digest()),
        # NRM partition lookup
        au.BoxReference(app_id=negrisk_mock.app_id, name=b"p_" + CONDITION_ID),
        # CTF — NRM mints YES/NO to adapter, adapter batch-transfers to orch,
        # orch h1-distributes to wallets
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + no_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        # NegRisk path touches one more foreign app than the plain CTF
        # adapter case (NRM). Pin only h1 + adapter on the dance txn —
        # algokit auto-populate piles inner-call refs onto whichever pad
        # already references their app, so seeding the dance txn is for
        # the orch-side calls; leaf-app refs (ct/ctf/usdce/negrisk_mock)
        # land on pads via the inner_boxes pre-pinning.
        extra_app_refs=[adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 0


def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint_Fees(
    split_negrisk_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Mint with fees: bob 52.5M pUSD, carla 50.1M pUSD, feeReceiver gets
    2.6M."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    taker_fee = 2_500_000
    maker_fee = 100_000
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + taker_fee,
    )
    _fund_wallet_pusd(
        wallet_psol=carla_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + maker_fee,
    )
    set_usdce_allowance(
        usdce, decode_address(vault.address),
        algod_addr_bytes_for_app(ct.app_id), 1_000_000_000,
    )

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    nrm_addr = algod_addr_bytes_for_app(negrisk_mock.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(orch_addr + adapter_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + orch_addr),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + ct_addr),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + fee_receiver),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + decode_address(vault.address)),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + adapter_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + nrm_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + ct_addr),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"a_" + sha256(adapter_addr + nrm_addr).digest()),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"a_" + sha256(ct_addr + adapter_addr).digest()),
        au.BoxReference(app_id=negrisk_mock.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + no_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        # NegRisk path touches one more foreign app than the plain CTF
        # adapter case (NRM). Pin only h1 + adapter on the dance txn —
        # algokit auto-populate piles inner-call refs onto whichever pad
        # already references their app, so seeding the dance txn is for
        # the orch-side calls; leaf-app refs (ct/ctf/usdce/negrisk_mock)
        # land on pads via the inner_boxes pre-pinning.
        extra_app_refs=[adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 0
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


def test_MatchOrdersNegRiskCtfCollateralAdapter_Complementary(
    split_negrisk_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Bob BUY YES + Carla SELL YES — direct P2P, no adapter call."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000,
    )
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, yes_id, 100_000_000)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[ct.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 100_000_000
    assert call(ctf, "balanceOf", [carla_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000


def test_MatchOrdersNegRiskCtfCollateralAdapter_Complementary_Fees(
    split_negrisk_adapter_with_delegate, admin, localnet,
    collateral_onramp_wired, vault,
):
    """Complementary with fees."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    taker_fee = 2_500_000
    maker_fee = 100_000
    _fund_wallet_pusd(
        wallet_psol=bob_psol, ct=ct, usdce=usdce,
        onramp=collateral_onramp_wired, vault=vault,
        h1_app_id=h1.app_id, admin=admin, amount=50_000_000 + taker_fee,
    )
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, yes_id, 100_000_000)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ct.app_id,
                        name=b"a_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + fee_receiver),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        extra_app_refs=[ct.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ct, "balanceOf", [bob_psol]) == 0
    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 100_000_000
    assert call(ctf, "balanceOf", [carla_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000 - maker_fee
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge(
    split_negrisk_adapter_with_delegate, admin, localnet, vault,
):
    """Both SELL YES+NO → adapter merges via NRM → 100M USDCe → CT.wrap →
    distribute 50M pUSD each."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    # NRM.mergePositions returns USDCe to msg.sender (= adapter). Pre-fund
    # the NRM mock's USDCe pool so it has collateral to refund.
    nrm_addr = algod_addr_bytes_for_app(negrisk_mock.app_id)
    deal_usdce(usdce, nrm_addr, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)

    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    # Same Merge requirement as the plain CTF adapter case: orch's CTF
    # approval slot is keyed (orch_algod, adapter_algod) and isn't pre-set
    # by Assets ctor (which uses adapter_psol).
    set_approval(ctf, orch_addr, adapter_addr, True)

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(orch_addr + adapter_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + no_bytes).digest()),
        au.BoxReference(app_id=negrisk_mock.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + nrm_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + adapter_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + ct_addr),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + decode_address(vault.address)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + orch_addr),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=100_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        # NegRisk path touches one more foreign app than the plain CTF
        # adapter case (NRM). Pin only h1 + adapter on the dance txn —
        # algokit auto-populate piles inner-call refs onto whichever pad
        # already references their app, so seeding the dance txn is for
        # the orch-side calls; leaf-app refs (ct/ctf/usdce/negrisk_mock)
        # land on pads via the inner_boxes pre-pinning.
        extra_app_refs=[adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [bob_psol]) == 50_000_000
    assert call(ctf, "balanceOf", [carla_psol, no_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000


def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge_Fees(
    split_negrisk_adapter_with_delegate, admin, localnet, vault,
):
    """Merge with fees: bob nets 49M, carla 49.5M, feeReceiver 1.5M."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    nrm_addr = algod_addr_bytes_for_app(negrisk_mock.app_id)
    deal_usdce(usdce, nrm_addr, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    ct_addr = algod_addr_bytes_for_app(ct.app_id)

    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    set_approval(ctf, orch_addr, adapter_addr, True)

    taker_fee = 1_000_000
    maker_fee = 500_000
    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    fee_receiver = addr(admin)
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(orch_addr + adapter_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(adapter_addr + no_bytes).digest()),
        au.BoxReference(app_id=negrisk_mock.app_id, name=b"p_" + CONDITION_ID),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + nrm_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + adapter_addr),
        au.BoxReference(app_id=usdce.app_id, name=b"b_" + ct_addr),
        au.BoxReference(app_id=usdce.app_id,
                        name=b"b_" + decode_address(vault.address)),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + orch_addr),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + bob_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + carla_psol),
        au.BoxReference(app_id=ct.app_id, name=b"b_" + fee_receiver),
    ]

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=100_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=taker_fee, maker_fee_amounts=[maker_fee],
        # NegRisk path touches one more foreign app than the plain CTF
        # adapter case (NRM). Pin only h1 + adapter on the dance txn —
        # algokit auto-populate piles inner-call refs onto whichever pad
        # already references their app, so seeding the dance txn is for
        # the orch-side calls; leaf-app refs (ct/ctf/usdce/negrisk_mock)
        # land on pads via the inner_boxes pre-pinning.
        extra_app_refs=[adapter.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
        budget_pad=15,
        per_pad_boxes=1,
    )

    assert call(ctf, "balanceOf", [bob_psol, yes_id]) == 0
    assert call(ct, "balanceOf", [bob_psol]) == 50_000_000 - taker_fee
    assert call(ctf, "balanceOf", [carla_psol, no_id]) == 0
    assert call(ct, "balanceOf", [carla_psol]) == 50_000_000 - maker_fee
    assert call(ct, "balanceOf", [fee_receiver]) == taker_fee + maker_fee


# ── revert paths ───────────────────────────────────────────────────────


def test_MatchOrdersNegRiskCtfCollateralAdapter_Merge_Reverts_WhenAdapterNotApproved(
    split_negrisk_adapter_with_delegate, admin, localnet, vault,
):
    """Merge needs orch's setApprovalForAll(adapter, true) on CTF for the
    adapter's batch transfer of YES+NO from orch to adapter. Skip the
    approval and matchOrders reverts."""
    (h1, _, orch, ct, ctf, usdce, adapter, negrisk_mock,
     chunk) = split_negrisk_adapter_with_delegate

    nrm_addr = algod_addr_bytes_for_app(negrisk_mock.app_id)
    deal_usdce(usdce, nrm_addr, 200_000_000)

    (bob_signer, carla_signer, bob_wallet, carla_wallet,
     bob_psol, carla_psol, yes_id, no_id) = _wallets_and_ids(
         localnet, admin, h1, negrisk_mock)
    _setup_negrisk_partition(negrisk_mock, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    adapter_addr = algod_addr_bytes_for_app(adapter.app_id)
    _fund_wallet_outcome(ctf, bob_psol, h1_addr, yes_id, 100_000_000)
    _fund_wallet_outcome(ctf, carla_psol, h1_addr, no_id, 100_000_000)
    # NB: deliberately skip set_approval(ctf, orch_algod, adapter_algod, True).

    taker = make_order(
        maker=bob_psol, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    maker = make_order(
        maker=carla_psol, token_id=no_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL, signature_type=SignatureType.POLY_1271,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    # Pre-list the same boxes as the happy Merge so auto-populate has
    # room to spread refs across pads. Without this it tries to pile
    # everything onto the dance txn during simulation and trips the
    # ref limit before the real logic-level revert can fire.
    yes_bytes = yes_id.to_bytes(32, "big")
    no_bytes = no_id.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=bob_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=carla_wallet.app_id, name=b"__dyn_storage"),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bob_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(carla_psol + h1_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(orch_addr + adapter_addr).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bob_psol + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(carla_psol + no_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(orch_addr + no_bytes).digest()),
    ]

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=100_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[adapter.app_id, h1.app_id],
            extra_box_refs=inner_boxes,
            budget_pad=15,
            per_pad_boxes=1,
        )


XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE = (
    "WhenAdapterUsdceMismatch needs a fully parallel exchange (h1/h2/orch/"
    "chunk) deployed against a bad NegRisk adapter whose USDCE/wcol differ "
    "from orch.ctfCollateral. Same shape as the plain-CTF variant's xfail. "
    "Tracked separately."
)


@pytest.mark.xfail(reason=XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE, strict=True)
def test_MatchOrdersNegRiskCtfCollateralAdapter_Mint_Reverts_WhenAdapterUsdceMismatch():
    raise NotImplementedError(XFAIL_MISMATCH_NEEDS_PARALLEL_EXCHANGE)
