"""Translation of v2 src/test/MatchOrders.t.sol + BalanceDeltas.t.sol.

End-to-end matchOrders settlement through the lonely-chunk dance:

    test → chunk.dance_call_7(matchOrders_sel, args)
              → itxn UpdateApplication(orch, helper3_bytes)
              → itxn ApplicationCall(orch.matchOrders, args) — runs in helper3
                  → itxn USDC.transferFrom / CTF.safeTransferFrom / split / merge
              → itxn UpdateApplication(orch, orch_orig_bytes)

Real USDC + CTF mocks (delegate/usdc_mock.py + delegate/ctf_mock.py) hold
balances; we deal + setAllowance/setApproval directly via test cheats
since maker/taker eth-style identities have no algorand private key.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from conftest import addr, app_id_to_address
from dev.addrs import algod_addr_bytes_for_app
from dev.deals import (
    deal_usdc, deal_usdc_and_approve,
    deal_outcome, deal_outcome_and_approve,
    prepare_condition, set_allowance, set_approval,
    usdc_balance, ctf_balance,
)
from dev.match_dispatch import dance_match_orders
from dev.orders import make_order, sign_order, hash_order_via_contract, Side, SignatureType
from dev.signing import bob, carla


# Test market conditionId. The YES/NO token IDs are not arbitrary —
# matchOrders validates them against `CTHelpers.getPositionId(ctfCollateral,
# getCollectionId(0, conditionId, indexSet))`. We compute the canonical
# values via helper1 in `_canonical_yes_no_ids` below.
CONDITION_ID = b"\xc0" * 32
# Provisional values for revert tests (where tokenIds aren't checked
# against the canonical partition before the revert fires). Happy paths
# require the values from `_canonical_yes_no_ids` instead.
YES_ID = 0xA1A1A1A1A1A1A1A1
NO_ID  = 0xB2B2B2B2B2B2B2B2


def _canonical_yes_no_ids(orch, h1):
    """Query helper1 for the YES/NO position IDs that matchOrders will
    compare maker/taker tokenIds against."""
    raw = _call_orch(orch, "getCtfCollateral")
    if isinstance(raw, str):
        from algosdk.encoding import decode_address
        ctf_collateral = decode_address(raw)
    else:
        ctf_collateral = bytes(raw)
    yes = _get_position_id(h1, orch, ctf_collateral, CONDITION_ID, 1)
    no = _get_position_id(h1, orch, ctf_collateral, CONDITION_ID, 2)
    return yes, no


def _call_orch(orch, method, args=None):
    return orch.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return


def _get_position_id(h1, orch, collateral_addr32: bytes, condition_id: bytes,
                     index_set: int) -> int:
    """Call helper1.CTHelpers.getCollectionId then getPositionId to get
    the canonical position id for an indexSet. Helper1 has --ensure-budget
    injected at the start of each library method, so a single call with
    sufficient extra_fee is enough."""
    coll_id = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getCollectionId",
        args=[list(b"\x00" * 32), list(condition_id), index_set],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    coll_id_bytes = bytes(coll_id) if isinstance(coll_id, (list, tuple)) else (
        bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id)
    pid = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getPositionId",
        args=[bytes(collateral_addr32), list(coll_id_bytes)],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    return int(pid)


def _operator_addr32(chunk):
    """The chunk's app address — registered as an operator in
    `split_settled_with_delegate`. msg.sender for the inner matchOrders
    call equals this address."""
    return bytes(app_id_to_address(chunk.app_id))


def _setup(orch, usdc, ctf, chunk, *, deal_table):
    """Common setup: register YES/NO partition, deal balances + approvals.

    `deal_table` is a list of dicts, each with:
      account: 32-byte address
      usdc: amount of USDC to deal + approve to the orch
      outcome: (token_id, amount) of outcome tokens to deal + approve.

    Approvals are set against the orch's *real* algorand account address
    (the one msg.sender resolves to inside inner-call execution), NOT the
    puya-sol `app_id_to_address` convention used for storage slots.
    """
    prepare_condition(ctf, CONDITION_ID, YES_ID, NO_ID)
    orch_addr = algod_addr_bytes_for_app(orch.app_id)
    for entry in deal_table:
        if entry.get("usdc"):
            deal_usdc_and_approve(usdc, entry["account"], orch_addr, entry["usdc"])
        if entry.get("outcome"):
            tid, amt = entry["outcome"]
            deal_outcome_and_approve(ctf, entry["account"], orch_addr, tid, amt)


# ── MatchOrders.t.sol — happy paths ────────────────────────────────────


SETTLEMENT_INFRA = (
    "matchOrders happy-path settlement through `dance_call_7`. The dance "
    "(test → chunk → orch[+chunk-bytes via UpdateApplication] → matchOrders "
    "→ helper1.transferFromERC1155 → CTFMock.safeTransferFrom) reaches "
    "CTFMock's `bal >= amt` assert at depth 4 and reads the carla balance "
    "box as 32 zero bytes — even though direct algod query of that exact "
    "box returns 100M and the depth-2 helper1.transferFromERC1155 path "
    "works correctly. Same failure on simulate AND real algod (TransactionPool.Remember). "
    "Diagnostics ruled out: wrong key, wrong inner-call args, simulate snapshots, "
    "populate iteration, app escrow funding, AVM `select` semantics, opcode-budget "
    "exhaustion, OpUp txn count. The actual mechanism that makes the box read return "
    "zero at depth 4 (post-UpdateApplication) is still undetermined."
)


def test_match_orders_complementary(split_settled_with_delegate):
    """test_MatchOrders_Complementary: bob (BUY 50 USDC for 100 YES) vs
    carla (SELL 100 YES for 50 USDC). Direct P2P transfer."""
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob()
    carla_signer = carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    yes_id, no_id = _canonical_yes_no_ids(orch, h1)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    # USDC/CTF inner-calls are dispatched through TransferHelper, which
    # lives in helper1 — so the immediate Txn.sender they observe is h1's
    # account, not the orch's. Approvals must be set against h1.
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 50_000_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)
    # Sanity: post-mint, before dance, the boxes that matchOrders will read.
    assert ctf_balance(ctf, carla_addr, yes_id) == 100_000_000
    assert usdc_balance(usdc, bob_addr) == 50_000_000

    taker = make_order(
        maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY,
    )
    maker = make_order(
        maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    # CTF/USDC inner-call boxes don't get auto-populated through deep itxns.
    # Pre-list the carla/bob balance + h1-approval boxes; dance_match_orders
    # spreads them across the pad calls.
    import algokit_utils as au
    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
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
        extra_box_refs=inner_boxes,
    )

    # Bob spent 50 USDC, received 100 YES.
    assert usdc_balance(usdc, bob_addr) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    # Carla spent 100 YES, received 50 USDC.
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 50_000_000


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_mint(split_settled_with_delegate):
    """test_MatchOrders_Mint: both BUY against complementary tokens.
    Exchange splits collateral into outcome tokens, distributes."""
    pytest.fail("MINT path — splitPosition flow")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_merge(split_settled_with_delegate):
    """test_MatchOrders_Merge: both SELL against complementary tokens.
    Exchange merges outcome tokens back to collateral, distributes."""
    pytest.fail("MERGE path — mergePositions flow")


def test_match_orders_complementary_fees(split_settled_with_delegate):
    """test_MatchOrders_Complementary_Fees: complementary settlement with
    maker + taker fees.

    Bob BUYs 100 YES for 50 USDC + 2.5 USDC taker fee (52.5 spent total).
    Carla SELLs 100 YES for 50 USDC, of which 0.1 USDC is the maker fee
    (49.9 received). FeeReceiver pockets 2.6 USDC."""
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob()
    carla_signer = carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    yes_id, no_id = _canonical_yes_no_ids(orch, h1)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 52_500_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)

    # Resolve the orch's feeReceiver via getter; needed for the post-trade
    # balance assertion + the foreign-app/box references.
    fee_receiver_raw = orch.send.call(au.AppClientMethodCallParams(
        method="getFeeReceiver", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(fee_receiver_raw, str):
        from algosdk.encoding import decode_address
        fee_receiver = decode_address(fee_receiver_raw)
    else:
        fee_receiver = bytes(fee_receiver_raw)

    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    maker = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
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
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(fee_receiver)),
    ]
    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=2_500_000,
        maker_fee_amounts=[100_000],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
    )

    # Taker: spent 52.5 USDC, received 100 YES.
    assert usdc_balance(usdc, bob_addr) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    # Maker: spent 100 YES, received 49.9 USDC (50 - 0.1 maker fee).
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 49_900_000
    # FeeReceiver pocketed 2.5 + 0.1 = 2.6 USDC.
    assert usdc_balance(usdc, fee_receiver) == 2_500_000 + 100_000


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_mint_fees(split_settled_with_delegate):
    """test_MatchOrders_Mint_Fees: MINT path with fees."""
    pytest.fail("mint with fees")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_match_orders_merge_fees(split_settled_with_delegate):
    """test_MatchOrders_Merge_Fees: MERGE path with fees."""
    pytest.fail("merge with fees")


@pytest.mark.xfail(
    reason="AVM-port architectural gap: taker-SELL settlement routes USDC "
           "through `address(this)` (orch) using `token.transfer` from the "
           "exchange to bob/feeReceiver. In Solidity msg.sender at USDC == "
           "exchange, but in the AVM port helper1 is the immediate caller, "
           "so USDC sees helper1 as sender — and helper1 has no balance. "
           "Fix path: helper3's `_transferCollateral(from=this, ...)` "
           "should emit `_transferFromERC20` with from=orch + a "
           "pre-set orch->helper1 approval, mirroring the Solidity flow.",
    strict=False,
)
def test_match_orders_complementary_fees_surplus(split_settled_with_delegate):
    """test_MatchOrders_Complementary_Fees_Surplus: taker SELLs 100 YES at
    50c (offers 100 YES for 50 USDC), maker BUYs at 60c (offers 60 USDC
    for 100 YES + 0.1 maker fee). Maker's price (60c) is *above* taker's
    (50c) so the cross is profitable for the taker — they receive the
    full 60 USDC (less their 2.5 taker fee) and the protocol routes
    carla's collateral through the exchange (`address(this)` in the
    contract) before paying out the taker proceeds and batched fees."""
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob()
    carla_signer = carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    yes_id, no_id = _canonical_yes_no_ids(orch, h1)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    # Roles flip vs complementary: bob holds YES (he's the seller), carla
    # holds USDC (she's the buyer with the 0.1 fee buffer).
    deal_outcome_and_approve(ctf, bob_addr, h1_addr, yes_id, 100_000_000)
    deal_usdc_and_approve(usdc, carla_addr, h1_addr, 60_100_000)

    fee_receiver_raw = orch.send.call(au.AppClientMethodCallParams(
        method="getFeeReceiver", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(fee_receiver_raw, str):
        from algosdk.encoding import decode_address
        fee_receiver = decode_address(fee_receiver_raw)
    else:
        fee_receiver = bytes(fee_receiver_raw)

    # The orch's algod account holds USDC briefly between maker→exchange
    # and exchange→{taker,feeReceiver} legs of `_settleComplementaryMaker`'s
    # taker-SELL else-branch. Pre-list its balance box so the inner
    # USDC.transferFrom reaches it.
    orch_addr = algod_addr_bytes_for_app(orch.app_id)

    # bob SELL 100 YES with 2.5 USDC taker fee.
    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    # carla BUY 100 YES at 60c (overpay) with 0.1 USDC maker fee.
    maker = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=60_000_000, taker_amount=100_000_000, side=Side.BUY)
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
        # CTF transfer bob -> carla
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(bob_addr) + h1_addr).digest()),
        # USDC transferFrom carla -> exchange (orch's account), then
        # exchange -> bob and exchange -> feeReceiver.
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(carla_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(carla_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(orch_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(fee_receiver)),
    ]
    dance_match_orders(
        chunk, orch,
        condition_id=CONDITION_ID,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=100_000_000,
        maker_fill_amounts=[60_000_000],
        taker_fee_amount=2_500_000,
        maker_fee_amounts=[100_000],
        extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
        extra_box_refs=inner_boxes,
    )

    # Taker: spent 100 YES, received 57.5 USDC (60 from carla - 2.5 fee).
    assert ctf_balance(ctf, bob_addr, yes_id) == 0
    assert usdc_balance(usdc, bob_addr) == 57_500_000
    # Maker: spent 60.1 USDC, received 100 YES.
    assert usdc_balance(usdc, carla_addr) == 0
    assert ctf_balance(ctf, carla_addr, yes_id) == 100_000_000
    # Fees: 2.5 + 0.1 = 2.6 USDC.
    assert usdc_balance(usdc, fee_receiver) == 2_500_000 + 100_000
    # Exchange's intermediate USDC balance ends at 0.
    assert usdc_balance(usdc, orch_addr) == 0


def test_match_orders_taker_refund(split_settled_with_delegate):
    """test_MatchOrders_TakerRefund: taker offers more than the maker
    needs (50 USDC for 100 YES at 50c), maker is at a better price (40c),
    so only 40 USDC is actually transferred — the 10 USDC excess stays
    in the taker's wallet (no refund txn needed; the protocol simply
    doesn't pull beyond what the maker fills)."""
    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob()
    carla_signer = carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    yes_id, no_id = _canonical_yes_no_ids(orch, h1)
    prepare_condition(ctf, CONDITION_ID, yes_id, no_id)

    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 50_000_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)

    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    # Maker is at 40c — strictly better than taker's 50c limit.
    maker = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=40_000_000, side=Side.SELL)
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    from hashlib import sha256
    yes_bytes = yes_id.to_bytes(32, "big")
    inner_boxes = [
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
        extra_box_refs=inner_boxes,
    )

    # Taker: sent 40 USDC (not 50 — maker's 40c price prevailed), got 100 YES.
    # The remaining 10 USDC stays in bob's wallet (not pulled).
    assert usdc_balance(usdc, bob_addr) == 10_000_000
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
    # Maker: spent 100 YES, received 40 USDC.
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert usdc_balance(usdc, carla_addr) == 40_000_000


# ── Revert tests ────────────────────────────────────────────────────────


def test_match_orders_revert_fee_exceeds_proceeds(split_settled_with_delegate):
    """test_MatchOrders_revert_FeeExceedsProceeds: a taker fee that's
    larger than the maker amount reverts before any settlement."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob()
    carla_signer = carla()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    _setup(orch, usdc, ctf, chunk, deal_table=[
        {"account": bob_addr, "usdc": 50_000_000},
        {"account": carla_addr, "outcome": (YES_ID, 100_000_000)},
    ])

    taker = make_order(
        maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000,
        side=Side.BUY,
    )
    maker = make_order(
        maker=carla_addr, token_id=YES_ID,
        maker_amount=100_000_000, taker_amount=50_000_000,
        side=Side.SELL,
    )
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=999_999_999,    # absurdly large fee
            maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_revert_not_crossing_sells(split_settled_with_delegate):
    """test_MatchOrders_revert_NotCrossingSells: two SELLs on different
    sides won't match (both want USDC, neither has the other's token)."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer, carla_signer = bob(), carla()
    bob_addr, carla_addr = bob_signer.eth_address_padded32, carla_signer.eth_address_padded32

    _setup(orch, usdc, ctf, chunk, deal_table=[
        {"account": bob_addr, "outcome": (YES_ID, 100_000_000)},
        {"account": carla_addr, "outcome": (NO_ID, 100_000_000)},
    ])

    yes_sell = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=100_000_000, taker_amount=60_000_000, side=Side.SELL)
    no_sell = make_order(maker=carla_addr, token_id=NO_ID,
        maker_amount=100_000_000, taker_amount=60_000_000, side=Side.SELL)
    t = sign_order(orch, yes_sell, bob_signer)
    m = sign_order(orch, no_sell, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=t.to_abi_list(),
            maker_orders_list=[m.to_abi_list()],
            taker_fill_amount=100_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_revert_not_crossing_buys(split_settled_with_delegate):
    """test_MatchOrders_revert_NotCrossingBuys: two BUYs at non-crossing
    prices on different tokens. 50c+40c=90c < 100c, fails crossing check."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer, carla_signer = bob(), carla()
    bob_addr, carla_addr = bob_signer.eth_address_padded32, carla_signer.eth_address_padded32

    _setup(orch, usdc, ctf, chunk, deal_table=[
        {"account": bob_addr, "usdc": 50_000_000},
        {"account": carla_addr, "usdc": 40_000_000},
    ])

    yes_buy = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    no_buy = make_order(maker=carla_addr, token_id=NO_ID,
        maker_amount=40_000_000, taker_amount=100_000_000, side=Side.BUY)
    t = sign_order(orch, yes_buy, bob_signer)
    m = sign_order(orch, no_buy, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=t.to_abi_list(),
            maker_orders_list=[m.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[40_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_revert_not_crossing_buy_vs_sell(split_settled_with_delegate):
    """test_MatchOrders_revert_NotCrossingBuyVsSell: 50c BUY vs 60c SELL,
    same token. Prices don't cross."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer, carla_signer = bob(), carla()
    bob_addr, carla_addr = bob_signer.eth_address_padded32, carla_signer.eth_address_padded32

    _setup(orch, usdc, ctf, chunk, deal_table=[
        {"account": bob_addr, "usdc": 50_000_000},
        {"account": carla_addr, "outcome": (YES_ID, 100_000_000)},
    ])

    buy = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    sell = make_order(maker=carla_addr, token_id=YES_ID,
        maker_amount=100_000_000, taker_amount=60_000_000, side=Side.SELL)
    t = sign_order(orch, buy, bob_signer)
    m = sign_order(orch, sell, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=t.to_abi_list(),
            maker_orders_list=[m.to_abi_list()],
            taker_fill_amount=0,
            maker_fill_amounts=[0],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


def test_match_orders_revert_invalid_trade(split_settled_with_delegate):
    """test_MatchOrders_revert_InvalidTrade: YES BUY vs NO SELL — the
    settlement path can't classify (neither complementary nor MINT/MERGE
    on these tokens) → MismatchedTokenIds."""
    _, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer, carla_signer = bob(), carla()
    bob_addr, carla_addr = bob_signer.eth_address_padded32, carla_signer.eth_address_padded32

    _setup(orch, usdc, ctf, chunk, deal_table=[
        {"account": bob_addr, "usdc": 50_000_000},
        {"account": carla_addr, "outcome": (NO_ID, 100_000_000)},
    ])

    buy = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    sell = make_order(maker=carla_addr, token_id=NO_ID,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    t = sign_order(orch, buy, bob_signer)
    m = sign_order(orch, sell, carla_signer)

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=CONDITION_ID,
            taker_order_list=t.to_abi_list(),
            maker_orders_list=[m.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0, maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id],
        )


# ── BalanceDeltas.t.sol — multi-maker variants ─────────────────────────


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_balance_deltas_multimaker_complementary_taker_buy(split_settled_with_delegate):
    """test_BalanceDeltas_MultiMaker_Complementary_TakerBuy"""
    pytest.fail("multi-maker")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_balance_deltas_multimaker_complementary_taker_sell(split_settled_with_delegate):
    """test_BalanceDeltas_MultiMaker_Complementary_TakerSell"""
    pytest.fail("multi-maker")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_balance_deltas_multimaker_complementary_with_fees(split_settled_with_delegate):
    """test_BalanceDeltas_MultiMaker_Complementary_WithFees"""
    pytest.fail("multi-maker fees")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_balance_deltas_multimaker_mint(split_settled_with_delegate):
    """test_BalanceDeltas_MultiMaker_Mint"""
    pytest.fail("multi-maker mint")


@pytest.mark.xfail(reason=SETTLEMENT_INFRA, strict=False)
def test_balance_deltas_multimaker_merge(split_settled_with_delegate):
    """test_BalanceDeltas_MultiMaker_Merge"""
    pytest.fail("multi-maker merge")
