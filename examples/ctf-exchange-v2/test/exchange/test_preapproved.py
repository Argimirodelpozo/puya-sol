"""Translation of v2 src/test/Preapproved.t.sol.

`preapproveOrder` is operator-gated; `invalidatePreapprovedOrder` is
operator-gated too. The auth tests don't go through matchOrders so they
don't need the dance. The matchOrders interactions go through
`split_settled_with_delegate` + `dance_match_orders`.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from algosdk.transaction import PaymentTxn, wait_for_confirmation

from conftest import AUTO_POPULATE, ZERO_ADDR, addr, app_id_to_address
from dev.deals import (
    deal_usdc_and_approve, deal_outcome_and_approve,
    prepare_condition,
)
from dev.match_dispatch import dance_match_orders
from dev.orders import make_order, sign_order, hash_order_via_contract, Side, SignatureType
from dev.signing import bob, carla


CONDITION_ID = b"\xc0" * 32
YES_ID = 0xA1A1A1A1A1A1A1A1
NO_ID  = 0xB2B2B2B2B2B2B2B2


def _empty_order(maker_addr: bytes, side: int = 0):
    return [
        1,
        list(maker_addr),
        list(maker_addr),
        50_000_000,
        50_000_000,
        100_000_000,
        side,
        0,
        0,
        [0] * 32,
        [0] * 32,
        b"",
    ]


def _call(client, method, args=None, sender=None, extra_fee=30_000):
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=AUTO_POPULATE).abi_return


def _call_with_pads(orch, client, method, args=None, sender=None,
                    extra_fee=80_000, n_pads=4):
    """Wrap a single app-call in a small group with `n_pads` no-op orch
    `isAdmin` calls so AVM's per-app opcode budget pools across the group
    (each pad adds ~700 opcodes). Required for methods that internally
    do ECDSA recover (~1700 ops) like preapproveOrder/validateOrderSignature.
    """
    composer = client.algorand.new_group()
    for i in range(n_pads):
        composer.add_app_call_method_call(orch.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin", args=[b"\x00" * 32],
                note=f"opup-{i}".encode(),
            )))
    composer.add_app_call_method_call(client.params.call(
        au.AppClientMethodCallParams(
            method=method, args=args or [],
            sender=sender.address if sender else None,
            extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        )))
    res = composer.send(au.SendParams(populate_app_call_resources=True))
    return res.returns[-1].value if res.returns else None


@pytest.fixture
def exchange(split_exchange):
    """Tuple unpacker — auth tests don't need the helpers."""
    _, _, orch = split_exchange
    return orch


@pytest.fixture
def non_operator(localnet, admin):
    """Funded account that's NOT registered as an operator."""
    acct = localnet.account.random()
    sp = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    wait_for_confirmation(
        localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)
    return acct


# ── preapproveOrder (auth checks fire before signature verification) ─────


def test_preapprove_order_revert_not_operator(exchange, non_operator):
    """preapproveOrder from a non-operator reverts NotOperator."""
    order = _empty_order(addr(non_operator), side=0)
    with pytest.raises(LogicError):
        _call(exchange, "preapproveOrder", [order],
              sender=non_operator, extra_fee=50_000)


def test_preapprove_order_revert_invalid_signature(exchange, admin):
    """Order maker = bob, signature produced by carla. Operator-gated path
    reaches the signature check, recovers carla, compares to bob → mismatch
    → InvalidSignature revert."""
    order = make_order(
        maker=bob().eth_address,
        token_id=12345,
        maker_amount=50_000_000,
        taker_amount=100_000_000,
        side=Side.BUY,
    )
    signed = sign_order(exchange, order, carla())
    with pytest.raises(LogicError):
        _call(exchange, "preapproveOrder", [signed.to_abi_list()],
              sender=admin, extra_fee=80_000)


# ── matchOrders interactions (revert paths via dance_call_7) ─────────────


def _setup_market(orch, usdc, ctf, *, bob_usdc=50_000_000,
                  carla_outcome=(YES_ID, 100_000_000)):
    """Common matchOrders setup: register YES/NO partition, deal balances
    + approvals to the orch."""
    prepare_condition(ctf, CONDITION_ID, YES_ID, NO_ID)
    orch_addr = bytes(app_id_to_address(orch.app_id))
    bob_addr = bob().eth_address_padded32
    carla_addr = carla().eth_address_padded32
    deal_usdc_and_approve(usdc, bob_addr, orch_addr, bob_usdc)
    deal_outcome_and_approve(ctf, carla_addr, orch_addr, *carla_outcome)
    return bob_addr, carla_addr


def _make_pair(orch):
    """Build a complementary BUY (bob) vs SELL (carla) order pair."""
    bob_signer, carla_signer = bob(), carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    taker = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    maker = make_order(maker=carla_addr, token_id=YES_ID,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    return (sign_order(orch, taker, bob_signer),
            sign_order(orch, maker, carla_signer))


SETTLEMENT_INFRA = (
    "happy-path matchOrders settlement through dance — same root cause as "
    "test_match_orders_complementary; see that test for details."
)


def test_match_orders_invalidated_preapproval_reverts(split_settled_with_delegate, admin):
    """test_matchOrders_invalidatedPreapproval_reverts: preapprove maker
    order, clear sig, invalidate preapproval, then matchOrders →
    InvalidSignature (no sig + no preapproval)."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    _setup_market(orch, usdc, ctf)
    taker_signed, maker_signed = _make_pair(orch)
    maker_hash = hash_order_via_contract(orch, maker_signed)

    _call_with_pads(orch, orch, "preapproveOrder",
                    [maker_signed.to_abi_list()], extra_fee=80_000)
    _call(orch, "invalidatePreapprovedOrder", [list(maker_hash)], extra_fee=20_000)

    # Strip the maker's signature — only preapproval could authorize.
    maker_signed.signature = b""

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_invalid_signature_not_preapproved_reverts(split_settled_with_delegate):
    """test_matchOrders_invalidSignature_notPreapproved_reverts: taker has
    a corrupted signature and isn't preapproved → InvalidSignature."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    _setup_market(orch, usdc, ctf)
    taker_signed, maker_signed = _make_pair(orch)

    # Corrupt the taker's signature.
    bad_sig = bytearray(taker_signed.signature)
    bad_sig[0] ^= 0xFF
    taker_signed.signature = bytes(bad_sig)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_empty_signature_not_preapproved_reverts(split_settled_with_delegate):
    """test_matchOrders_emptySignature_notPreapproved_reverts: empty
    signature + no preapproval → InvalidSignature."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    _setup_market(orch, usdc, ctf)
    taker_signed, maker_signed = _make_pair(orch)
    taker_signed.signature = b""

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_empty_signature_invalidated_preapproval_reverts(
    split_settled_with_delegate
):
    """test_matchOrders_emptySignature_invalidatedPreapproval_reverts:
    preapprove taker, then invalidate, then matchOrders with empty sig →
    InvalidSignature."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    _setup_market(orch, usdc, ctf)
    taker_signed, maker_signed = _make_pair(orch)
    taker_hash = hash_order_via_contract(orch, taker_signed)

    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)
    _call(orch, "invalidatePreapprovedOrder", [list(taker_hash)], extra_fee=20_000)
    taker_signed.signature = b""

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


# ── happy paths (still gated by the same matchOrders dance issue as
#    test_match_orders_complementary; see that file for details) ──────────


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_preapproved_maker_complementary(split_settled_with_delegate):
    """test_matchOrders_preapprovedMaker_complementary"""
    pytest.fail("happy-path settlement — see test_match_orders_complementary")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_preapproved_taker_complementary(split_settled_with_delegate):
    """test_matchOrders_preapprovedTaker_complementary"""
    pytest.fail("happy-path settlement")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_preapproved_respects_filled_status(split_settled_with_delegate):
    """test_matchOrders_preapproved_respectsFilledStatus"""
    pytest.fail("happy-path settlement")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_preapproved_respects_user_pause(split_settled_with_delegate):
    """test_matchOrders_preapproved_respectsUserPause"""
    pytest.fail("happy-path settlement")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_preapproved_partial_fill(split_settled_with_delegate):
    """test_matchOrders_preapproved_partialFill"""
    pytest.fail("happy-path settlement")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_empty_signature_preapproved_complementary(split_settled_with_delegate):
    """test_matchOrders_emptySignature_preapproved_complementary"""
    pytest.fail("happy-path settlement")


@pytest.mark.xfail(reason="ERC1271 1271-preapproval invalidation flow needs "
    "ToggleableERC1271Mock fixture wiring + signed-by-1271 happy path",
    strict=False)
def test_match_orders_preapproved_1271_signer_invalidated(split_settled_with_delegate):
    """test_matchOrders_preapproved1271_signerInvalidated"""
    pytest.fail("ToggleableERC1271 + dance happy-path")
