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


def _setup_complementary_market(orch, h1, ctf, usdc):
    """Common matchOrders complementary setup with canonical YES/NO IDs.
    Returns (yes_id, no_id, bob_addr, carla_addr, h1_addr)."""
    from dev.addrs import algod_addr_bytes_for_app
    raw = orch.send.call(au.AppClientMethodCallParams(
        method="getCtfCollateral", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(raw, str):
        from algosdk.encoding import decode_address
        ctf_collateral = decode_address(raw)
    else:
        ctf_collateral = bytes(raw)

    def _pid(idx_set):
        cid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getCollectionId",
            args=[list(b"\x00" * 32), list(CONDITION_ID), idx_set],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        cid_b = bytes(cid) if isinstance(cid, (list, tuple)) else bytes(cid)
        pid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getPositionId",
            args=[bytes(ctf_collateral), list(cid_b)],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        return int(pid)

    yes_id, no_id = _pid(1), _pid(2)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    bob_addr = bob().eth_address_padded32
    carla_addr = carla().eth_address_padded32
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 50_000_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)
    return yes_id, no_id, bob_addr, carla_addr, h1_addr


def _make_complementary_pair(orch, yes_id, bob_addr, carla_addr):
    """bob BUY 100 YES @ 50c vs carla SELL 100 YES @ 50c."""
    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    maker = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    return (sign_order(orch, taker, bob()),
            sign_order(orch, maker, carla()))


def _complementary_inner_box_refs(usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr):
    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")
    return [
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(bob_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(carla_addr)),
    ]


def test_match_orders_preapproved_maker_complementary(split_settled_with_delegate):
    """test_matchOrders_preapprovedMaker_complementary: same complementary
    settlement as `test_match_orders_complementary`, but the maker order
    is preapproved on-chain via `preapproveOrder` and its signature is
    cleared — only the on-chain preapproval authorizes the match."""
    from dev.deals import usdc_balance, ctf_balance
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)
    taker_signed, maker_signed = _make_complementary_pair(
        orch, yes_id, bob_addr, carla_addr)
    # Preapprove the maker, then clear its signature so only preapproval
    # authorizes its half of the match.
    _call_with_pads(orch, orch, "preapproveOrder",
                    [maker_signed.to_abi_list()], extra_fee=80_000)
    maker_signed.signature = b""

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0,
        maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=_complementary_inner_box_refs(
            usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr),
    )

    assert usdc_balance(usdc, bob_addr) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 50_000_000


def test_match_orders_preapproved_taker_complementary(split_settled_with_delegate):
    """test_matchOrders_preapprovedTaker_complementary: same complementary
    settlement, but the *taker* order is preapproved (signature cleared)."""
    from dev.deals import usdc_balance, ctf_balance
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)
    taker_signed, maker_signed = _make_complementary_pair(
        orch, yes_id, bob_addr, carla_addr)
    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)
    taker_signed.signature = b""

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0,
        maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=_complementary_inner_box_refs(
            usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr),
    )

    assert usdc_balance(usdc, bob_addr) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 50_000_000


def test_match_orders_preapproved_respects_filled_status(split_settled_with_delegate):
    """test_matchOrders_preapproved_respectsFilledStatus: a fully-filled
    preapproved order can't be matched again. First dance fills both
    sides, second dance with the same orders must revert with
    OrderAlreadyFilled."""
    from dev.deals import usdc_balance, ctf_balance
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)
    taker_signed, maker_signed = _make_complementary_pair(
        orch, yes_id, bob_addr, carla_addr)
    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)
    _call_with_pads(orch, orch, "preapproveOrder",
                    [maker_signed.to_abi_list()], extra_fee=80_000)
    taker_signed.signature = b""
    maker_signed.signature = b""

    box_refs = _complementary_inner_box_refs(
        usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr)

    # First match — fills both sides completely.
    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=box_refs,
    )
    assert usdc_balance(usdc, carla_addr) == 50_000_000
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000

    # Second match — taker's order is now filled. Should revert.
    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
            extra_box_refs=box_refs,
        )


def test_match_orders_preapproved_respects_user_pause(split_settled_with_delegate):
    """test_matchOrders_preapproved_respectsUserPause: when bob (the taker
    order's maker) is paused, the match must revert with UserIsPaused
    even if the order is preapproved.

    AVM-PORT-ADAPTATION: bob has no Algorand key — he's an eth-style
    EOA used as a maker identity, so we can't send `pauseUser()` as
    bob. The orch exposes `_avmPortPauseUser(address)` (admin-only)
    that writes the same storage slot `pauseUser()` does. Foundry's
    `vm.prank(bob); ct.pauseUser()` is the equivalent."""
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)
    taker_signed, maker_signed = _make_complementary_pair(
        orch, yes_id, bob_addr, carla_addr)

    # Preapprove the taker order so the signature gate doesn't fire
    # before the pause gate.
    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)

    # Pause bob via the admin cheat. Lower `userPauseBlockInterval` to 0
    # first so the pause takes effect immediately (the default is 100).
    _call_with_pads(orch, orch, "setUserPauseBlockInterval", [0])
    _call_with_pads(orch, orch, "_avmPortPauseUser", [bytes(bob_addr)])

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000, maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
            extra_box_refs=_complementary_inner_box_refs(
                usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr),
        )


def test_match_orders_preapproved_partial_fill(split_settled_with_delegate):
    """test_matchOrders_preapproved_partialFill: bob preapproves a BUY for
    100 YES at 50c, then it's filled in two halves. First match: carla
    sells 50 YES @ 25c (filling half). Second match: dave sells the
    remaining 50 YES @ 25c. Final: bob has 100 YES, 0 USDC."""
    from dev.deals import usdc_balance, ctf_balance, deal_outcome_and_approve
    from dev.signing import dave as dave_signer_fn
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)

    # Replace the carla deal — partial fill means carla only has 50 YES
    # (not the default 100 from `_setup_complementary_market`). The 100M
    # mint already happened, so we just continue — carla has 100M and we
    # fill half with her, half with dave (who needs to be set up).
    bob_signer = bob()
    carla_signer = carla()
    dave_signer = dave_signer_fn()
    dave_addr = dave_signer.eth_address_padded32
    deal_outcome_and_approve(ctf, dave_addr, h1_addr, yes_id, 50_000_000)

    # Bob's order: BUY 100 YES at 50c (the full size).
    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    taker_signed = sign_order(orch, taker, bob_signer)
    # Preapprove bob's order, clear sig — preapproval authorizes both partial
    # fills below.
    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)
    taker_signed.signature = b""

    # Carla's order: SELL 50 YES at 25c (matches half of bob).
    maker_carla = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=25_000_000, side=Side.SELL)
    maker_carla_signed = sign_order(orch, maker_carla, carla_signer)

    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")

    def _box_refs(other_addr):
        return [
            au.BoxReference(app_id=ctf.app_id,
                            name=b"b_" + sha256(bytes(other_addr) + yes_bytes).digest()),
            au.BoxReference(app_id=ctf.app_id,
                            name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
            au.BoxReference(app_id=ctf.app_id,
                            name=b"ap_" + sha256(bytes(other_addr) + h1_addr).digest()),
            au.BoxReference(app_id=usdc.app_id,
                            name=b"a_" + sha256(bytes(bob_addr) + h1_addr).digest()),
            au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
            au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(other_addr)),
        ]

    # First half: bob ↔ carla, 50 YES for 25 USDC.
    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_carla_signed.to_abi_list()],
        taker_fill_amount=25_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=_box_refs(carla_addr),
    )
    assert ctf_balance(ctf, bob_addr, yes_id) == 50_000_000
    assert usdc_balance(usdc, carla_addr) == 25_000_000

    # Second half: bob ↔ dave, 50 YES for 25 USDC.
    maker_dave = make_order(maker=dave_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=25_000_000, side=Side.SELL)
    maker_dave.salt = 2  # avoid hash collision with the carla order
    maker_dave_signed = sign_order(orch, maker_dave, dave_signer)

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_dave_signed.to_abi_list()],
        taker_fill_amount=25_000_000,
        maker_fill_amounts=[50_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=_box_refs(dave_addr),
    )

    # Bob's BUY order is now fully filled.
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    assert usdc_balance(usdc, bob_addr) == 0


def test_match_orders_empty_signature_preapproved_complementary(split_settled_with_delegate):
    """test_matchOrders_emptySignature_preapproved_complementary: BOTH the
    taker and maker orders are preapproved with their full signatures,
    then both signatures are cleared. The on-chain preapproval alone
    must authorize the match — same balances as the unpreapproved
    complementary case."""
    from dev.deals import usdc_balance, ctf_balance
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    yes_id, _no_id, bob_addr, carla_addr, h1_addr = _setup_complementary_market(
        orch, h1, ctf, usdc)
    taker_signed, maker_signed = _make_complementary_pair(
        orch, yes_id, bob_addr, carla_addr)

    # Preapprove both with their valid signatures, then clear both.
    _call_with_pads(orch, orch, "preapproveOrder",
                    [taker_signed.to_abi_list()], extra_fee=80_000)
    _call_with_pads(orch, orch, "preapproveOrder",
                    [maker_signed.to_abi_list()], extra_fee=80_000)
    taker_signed.signature = b""
    maker_signed.signature = b""

    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=_complementary_inner_box_refs(
            usdc, ctf, h1_addr, yes_id, bob_addr, carla_addr),
    )

    assert usdc_balance(usdc, bob_addr) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 50_000_000


def test_match_orders_preapproved_1271_signer_invalidated(
    split_settled_with_delegate, toggleable_1271_factory
):
    """test_matchOrders_preapproved1271_signerInvalidated:
    a preapproved POLY_1271 order remains matchable even after the
    wallet's 1271 validation is `disable()`d, while a different order
    from the same disabled wallet (not preapproved) reverts on the
    1271 path.

    AVM-PORT-ADAPTATION: maker is the wallet's puya-sol storage
    encoding (`\\x00*24 || itob(app_id)`) so the 1271 inner-call's
    `extract_uint64` at offset 24 resolves to the wallet's app id;
    USDC allowance and balance keys are stored against that same
    encoding so the orch's `transferFrom(wallet, exchange, …)` lookup
    lines up.
    """
    from dev.deals import (
        deal_usdc_and_approve, deal_outcome_and_approve, set_allowance,
        usdc_balance, ctf_balance,
    )
    from dev.addrs import algod_addr_bytes_for_app
    from hashlib import sha256

    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer, carla_signer = bob(), carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    # Deploy the toggleable wallet with carla as the inner signer.
    wallet = toggleable_1271_factory(carla_signer.eth_address)
    wallet_addr32 = app_id_to_address(wallet.app_id)  # psol storage encoding
    wallet_real = algod_addr_bytes_for_app(wallet.app_id)

    # Same yes/no derivation as `_setup_complementary_market`, inlined
    # because we want different deal targets (the wallet, not bob/carla
    # for the buy side).
    yes_id, no_id, _, _, h1_addr = _setup_complementary_market(orch, h1, ctf, usdc)

    # Wallet has 100M USDC and approves the exchange (h1 is the
    # `address(this)` of matchOrders' inner-call frame).
    deal_usdc_and_approve(usdc, wallet_addr32, h1_addr, 100_000_000)

    # Build two BUY orders from the same wallet, signed by carla via
    # POLY_1271. Different salts.
    preapproved = make_order(
        salt=1, maker=wallet_addr32, signer=wallet_addr32, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY,
        signature_type=SignatureType.POLY_1271,
    )
    preapproved_signed = sign_order(orch, preapproved, carla_signer)

    not_preapproved = make_order(
        salt=2, maker=wallet_addr32, signer=wallet_addr32, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY,
        signature_type=SignatureType.POLY_1271,
    )
    not_preapproved_signed = sign_order(orch, not_preapproved, carla_signer)

    # Preapprove only the first one while the wallet is still active.
    _call_with_pads(orch, orch, "preapproveOrder",
                    [preapproved_signed.to_abi_list()], extra_fee=80_000)

    # Disable the wallet — 1271 returns 0 from now on.
    wallet.send.call(au.AppClientMethodCallParams(
        method="disable", args=[],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
    ), send_params=AUTO_POPULATE)

    # ── Scenario 1: notPreapproved order from disabled wallet → revert ──
    # bob is the SELL-side maker (already has yes_id from
    # `_setup_complementary_market`).
    maker_for_fail = make_order(
        maker=bob_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL,
    )
    maker_for_fail_signed = sign_order(orch, maker_for_fail, bob_signer)
    yes_bytes = yes_id.to_bytes(32, "big")
    box_refs_fail = [
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(wallet_addr32) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(bob_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(wallet_addr32) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(wallet_addr32)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
    ]
    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=not_preapproved_signed.to_abi_list(),
            maker_orders_list=[maker_for_fail_signed.to_abi_list()],
            taker_fill_amount=50_000_000, maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id, wallet.app_id],
            extra_box_refs=box_refs_fail,
        )

    # ── Scenario 2: preapproved order with empty signature → succeeds ──
    preapproved_signed.signature = b""
    # carla is the SELL-side maker (already has yes_id from
    # `_setup_complementary_market`).
    maker_for_success = make_order(
        maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL,
    )
    maker_for_success_signed = sign_order(orch, maker_for_success, carla_signer)
    box_refs_success = [
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(wallet_addr32) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(wallet_addr32) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(wallet_addr32)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(carla_addr)),
    ]
    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=preapproved_signed.to_abi_list(),
        maker_orders_list=[maker_for_success_signed.to_abi_list()],
        taker_fill_amount=50_000_000, maker_fill_amounts=[100_000_000],
        taker_fee_amount=0, maker_fee_amounts=[0],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id, wallet.app_id],
        extra_box_refs=box_refs_success,
    )

    # Wallet now holds 100M YES and 50M USDC remains.
    assert ctf_balance(ctf, wallet_addr32, yes_id) == 100_000_000
    assert usdc_balance(usdc, wallet_addr32) == 50_000_000
