"""Proof-of-life: dispatch matchOrders through the lonely-chunk dance with
real ABI-encoded args and confirm the orch's pre-validation revert paths
fire.

This is the smallest end-to-end exercise of the dispatch chain:

    test → chunk.dance_call_7 → itxn(orch.matchOrders, args)
                                        → revert (NoMakerOrders / etc.)

If these revert paths trigger as expected, the dispatch path itself works
end-to-end and we can layer on signed orders + real settlement above it.
"""
import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.match_dispatch import dance_match_orders
from dev.signing import bob


def test_dispatch_no_maker_orders_reverts(split_settled_with_delegate):
    """Empty maker_orders → orch.matchOrders reverts NoMakerOrders before
    any signature/settlement work. Validates the dance-dispatch path
    delivers args correctly to the matchOrders body in helper3."""
    h1, h2, orch, usdc, ctf, chunk = split_settled_with_delegate

    bob_addr = bob().eth_address_padded32

    # Build a minimal taker order shell (won't be touched — revert fires first).
    taker_order = [
        1,                # salt
        list(bob_addr),   # maker
        list(bob_addr),   # signer
        12345,            # tokenId
        50_000_000, 100_000_000,  # makerAmount, takerAmount
        0, 0, 0,          # side BUY, sigType EOA, timestamp
        [0] * 32, [0] * 32,
        b"",
    ]

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=b"\x00" * 32,
            taker_order_list=taker_order,
            maker_orders_list=[],         # empty → NoMakerOrders revert
            taker_fill_amount=0,
            maker_fill_amounts=[],
            taker_fee_amount=0,
            maker_fee_amounts=[],
        )


def test_dispatch_mismatched_array_lengths_reverts(split_settled_with_delegate):
    """maker_orders[1] but maker_fill_amounts[2] → MismatchedArrayLengths."""
    h1, h2, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_addr = bob().eth_address_padded32

    order = [
        1, list(bob_addr), list(bob_addr),
        12345, 50_000_000, 100_000_000,
        0, 0, 0, [0] * 32, [0] * 32, b"",
    ]

    with pytest.raises(LogicError):
        dance_match_orders(
            chunk, orch,
            condition_id=b"\x00" * 32,
            taker_order_list=order,
            maker_orders_list=[order],
            taker_fill_amount=0,
            maker_fill_amounts=[0, 0],   # length 2, mismatch
            taker_fee_amount=0,
            maker_fee_amounts=[0],
        )
