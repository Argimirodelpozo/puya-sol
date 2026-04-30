"""Direct call to helper1.TransferHelper._transferFromERC1155 — bypasses
matchOrders entirely. If helper1 → CTFMock works here, the matchOrders
bal=0 issue is a dance-specific quirk; if it fails the same way, the
failure is in helper1's stub → CTFMock encoding."""
import algokit_utils as au
import pytest
from hashlib import sha256

from conftest import addr
from dev.addrs import algod_addr_bytes_for_app
from dev.deals import deal_outcome_and_approve, prepare_condition
from dev.signing import bob, carla


CONDITION_ID = b"\xc0" * 32


def _yes_id(orch, h1):
    raw = orch.send.call(au.AppClientMethodCallParams(
        method="getCtfCollateral", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(raw, str):
        from algosdk.encoding import decode_address
        ctf_collateral = decode_address(raw)
    else:
        ctf_collateral = bytes(raw)
    coll = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getCollectionId",
        args=[list(b"\x00" * 32), list(CONDITION_ID), 1],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    coll_b = bytes(coll) if isinstance(coll, (list, tuple, bytes)) else b""
    yes = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getPositionId",
        args=[bytes(ctf_collateral), list(coll_b)],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    return int(yes)


def test_helper1_direct_transferFromERC1155(split_settled_with_delegate):
    """Call helper1.TransferHelper._transferFromERC1155 directly with
    (token=ctf, from=carla, to=bob, id=yes_id, amount=100M). Should
    transfer 100M from carla's box to bob's box."""
    h1, _, orch, _, ctf, chunk = split_settled_with_delegate
    bob_addr = bob().eth_address_padded32
    carla_addr = carla().eth_address_padded32

    yes_id = _yes_id(orch, h1)
    prepare_condition(ctf, CONDITION_ID, yes_id, yes_id + 1)
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)

    # Pre-call sanity
    yes_bytes = yes_id.to_bytes(32, "big")
    carla_bal_box = b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()
    bob_bal_box = b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()
    carla_ap_box = b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()

    # Encode the address arg as 32-byte address (puya-sol's `address` =
    # 24-zero-prefix + 8-byte app-id when it represents an app)
    ctf_addr32 = b"\x00" * 24 + ctf.app_id.to_bytes(8, "big")

    # uint512-shape inputs: helper1 expects 64-byte ABI uint512 for
    # (id, amount). We pass 32-byte values; the sender pads to 64.
    res = h1.send.call(au.AppClientMethodCallParams(
        method="TransferHelper._transferFromERC1155",
        args=[ctf_addr32, carla_addr, bob_addr, yes_id, 100_000_000],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
        app_references=[ctf.app_id],
        box_references=[
            au.BoxReference(app_id=ctf.app_id, name=carla_bal_box),
            au.BoxReference(app_id=ctf.app_id, name=bob_bal_box),
            au.BoxReference(app_id=ctf.app_id, name=carla_ap_box),
        ],
    ), send_params=au.SendParams(populate_app_call_resources=True))

    # Verify post-transfer
    from dev.deals import ctf_balance
    assert ctf_balance(ctf, carla_addr, yes_id) == 0
    assert ctf_balance(ctf, bob_addr, yes_id) == 100_000_000
