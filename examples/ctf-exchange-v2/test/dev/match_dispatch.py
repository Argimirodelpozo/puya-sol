"""matchOrders dispatch through the lonely-chunk dance.

The orch's matchOrders body lives in helper3 (`--force-delegate matchOrders`
in compile_all.sh). At runtime, we install helper3's bytes onto the orch
via UpdateApplication, call orch with matchOrders' selector + args (the
orch's router now dispatches into helper3's body), then revert the orch's
program. All three steps run as itxns inside `LonelyChunk.dance_call_7`.

This module ABI-encodes the 7 matchOrders args + invokes
`chunk.dance_call_7(__delegate_update_sel, matchOrders_sel, a1..a7)`.
"""
import hashlib

import algokit_utils as au
from algosdk.abi import (
    ABIType,
    AddressType,
    ArrayDynamicType,
    ArrayStaticType,
    ByteType,
    TupleType,
    UintType,
)


def _sel(sig: str) -> bytes:
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


# Selectors
DELEGATE_UPDATE_SEL = bytes.fromhex("dc5e3798")  # __delegate_update()void
MATCH_ORDERS_SEL = _sel(
    "matchOrders(byte[32],"
    "(uint256,uint8[32],uint8[32],uint256,uint256,uint256,uint8,uint8,"
    "uint256,uint8[32],uint8[32],byte[]),"
    "(uint256,uint8[32],uint8[32],uint256,uint256,uint256,uint8,uint8,"
    "uint256,uint8[32],uint8[32],byte[])[],"
    "uint256,uint256[],uint256,uint256[])void"
)


# ARC-4 type strings for the 7 matchOrders args.
_ORDER_TYPE_STR = (
    "(uint256,uint8[32],uint8[32],uint256,uint256,uint256,uint8,uint8,"
    "uint256,uint8[32],uint8[32],byte[])"
)
_BYTE32 = ArrayStaticType(ByteType(), 32)
_ORDER_ABI = ABIType.from_string(_ORDER_TYPE_STR)
_ORDER_ARRAY_ABI = ArrayDynamicType(_ORDER_ABI)
_UINT256_ARRAY_ABI = ArrayDynamicType(UintType(256))
_UINT256_ABI = UintType(256)


def _to_uint8_list(v):
    """Coerce bytes / bytearray / list to a 32-element list[int] (the form
    algosdk's encoder for uint8[32] expects)."""
    if isinstance(v, (bytes, bytearray)):
        if len(v) != 32:
            raise ValueError(f"uint8[32] must be 32 bytes, got {len(v)}")
        return list(v)
    if isinstance(v, list):
        if len(v) != 32:
            raise ValueError(f"uint8[32]-as-list must have 32 elements, got {len(v)}")
        return list(v)
    raise TypeError(f"unexpected uint8[32] source type: {type(v).__name__}")


def _order_to_abi_tuple(order_list):
    """Convert `Order.to_abi_list()` to a tuple ready for ABIType.encode.

    The Order ABI uses `uint8[32]` for maker/signer/metadata/builder.
    algosdk parses that as ArrayStaticType(UintType(8), 32), whose encoder
    expects list[int] — not bytes. `to_abi_list()` already returns list[int]
    for these slots; this helper just normalizes types defensively.
    """
    o = list(order_list)
    for i in (1, 2, 9, 10):
        o[i] = _to_uint8_list(o[i])
    return tuple(o)


def encode_match_orders_args(
    *,
    condition_id: bytes,
    taker_order_list,
    maker_orders_list,
    taker_fill_amount: int,
    maker_fill_amounts: list,
    taker_fee_amount: int,
    maker_fee_amounts: list,
):
    """Encode each of the 7 matchOrders args as raw bytes (one per
    ApplicationArgs slot, post-selector).

    Returns a tuple `(a1, a2, a3, a4, a5, a6, a7)` of bytes."""
    if len(condition_id) != 32:
        raise ValueError(f"condition_id must be 32 bytes, got {len(condition_id)}")
    a1 = bytes(condition_id)
    a2 = _ORDER_ABI.encode(_order_to_abi_tuple(taker_order_list))
    a3 = _ORDER_ARRAY_ABI.encode(
        [_order_to_abi_tuple(o) for o in maker_orders_list])
    a4 = _UINT256_ABI.encode(taker_fill_amount)
    a5 = _UINT256_ARRAY_ABI.encode(maker_fill_amounts)
    a6 = _UINT256_ABI.encode(taker_fee_amount)
    a7 = _UINT256_ARRAY_ABI.encode(maker_fee_amounts)
    return a1, a2, a3, a4, a5, a6, a7


def dance_match_orders(
    chunk,
    orch,
    *,
    condition_id: bytes,
    taker_order_list,
    maker_orders_list,
    taker_fill_amount: int,
    maker_fill_amounts: list,
    taker_fee_amount: int,
    maker_fee_amounts: list,
    extra_app_refs=None,
    extra_box_refs=None,
    extra_fee: int = 5_000_000,
    budget_pad: int = 15,
):
    """Run the matchOrders dance through `chunk` against `orch`.

    `chunk` and `orch` are AppClients. Returns the chunk's return
    (last_log of step 2, which is matchOrders' return → empty for void).

    `budget_pad`: number of dummy app calls to add to the outer group
    so the matchOrders inner-call has enough opcode budget for two
    `ecdsa_pk_recover` ops (~1700 each) + the matchOrders body. AVM
    pools inner-call budget across all app calls in a group.
    """
    a1, a2, a3, a4, a5, a6, a7 = encode_match_orders_args(
        condition_id=condition_id,
        taker_order_list=taker_order_list,
        maker_orders_list=maker_orders_list,
        taker_fill_amount=taker_fill_amount,
        maker_fill_amounts=maker_fill_amounts,
        taker_fee_amount=taker_fee_amount,
        maker_fee_amounts=maker_fee_amounts,
    )
    composer = chunk.algorand.new_group()
    chunk_box_refs = [
        au.BoxReference(app_id=0, name=b"__self_bytes"),
        au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
    ]
    # Spread `extra_box_refs` (CTF/USDC inner-call boxes) across pad calls
    # to stay under the 8-ref/txn cap. Each pad call already has `orch` as
    # its target; we add the box's foreign app too so the box ref resolves.
    # Box availability is pooled across the txn group, so a box on pad-N
    # is reachable from the dance txn's inner calls.
    extra_boxes_list = list(extra_box_refs or [])
    # Per-pad cap: 6 refs (1 implicit orch + 1 foreign app + 6 boxes = 8 max)
    per_pad_boxes = 6
    box_idx = 0
    for i in range(budget_pad):
        slot_boxes = extra_boxes_list[box_idx:box_idx + per_pad_boxes]
        box_idx += per_pad_boxes
        slot_app_ids = sorted({b.app_index for b in slot_boxes if b.app_index != 0})
        composer.add_app_call_method_call(orch.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin",
                args=[b"\x00" * 32],
                note=f"opup-{i}".encode(),
                box_references=slot_boxes if slot_boxes else None,
                app_references=slot_app_ids if slot_app_ids else None,
            )))
    composer.add_app_call_method_call(chunk.params.call(
        au.AppClientMethodCallParams(
            method="dance_call_7",
            args=[DELEGATE_UPDATE_SEL, MATCH_ORDERS_SEL,
                  a1, a2, a3, a4, a5, a6, a7],
            extra_fee=au.AlgoAmount(micro_algo=extra_fee),
            app_references=[orch.app_id] + (extra_app_refs or []),
            box_references=chunk_box_refs,
        )))
    res = composer.send(au.SendParams(
        populate_app_call_resources=True,
    ))
    # Last item in returns is dance_call_7's abi return.
    return res.returns[-1].value if res.returns else None
