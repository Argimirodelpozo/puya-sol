"""Smoke tests for the delegate-update mechanism.

Exercises the install/call/revert ceremony end-to-end on localnet without
a real F (the migrated matchOrders body). The lonely chunk's
delegate_dance method:

  1. Reads its own approval bytes from __self_bytes box (4 × 2KB pages).
  2. UpdateApplication(orch, approval=self_bytes).
  3. (No-op call to orch — would dispatch to the migrated method body
     once it's merged into self.)
  4. UpdateApplication(orch, approval=orch_orig_bytes).

The mechanism is verified by:
  * deploy_and_populate works (sidecar + 2 boxes filled).
  * Calling delegate_dance succeeds.
  * Orch's approval bytes match the original after the dance (so the
    revert restored cleanly).
"""
import algokit_utils as au
import pytest
from algosdk import encoding as algosdk_encoding

from conftest import AUTO_POPULATE


def test_lonely_chunk_deploys_with_boxes(split_exchange_with_delegate):
    """split_exchange_with_delegate creates the sidecar, allocates both
    boxes via op.Box.create, and writes 4 × 2KB pages into each."""
    _, _, _, chunk = split_exchange_with_delegate
    assert chunk.app_id > 0


def test_delegate_dance_round_trip(split_exchange_with_delegate, localnet):
    """delegate_dance: install → call → revert. Verify orch's approval
    bytes are restored after."""
    _, _, orch, chunk = split_exchange_with_delegate
    algod = localnet.client.algod

    pre_info = algod.application_info(orch.app_id)
    pre_approval = pre_info["params"]["approval-program"]

    chunk.send.call(au.AppClientMethodCallParams(
        method="delegate_dance",
        args=[bytes.fromhex("dc5e3798")],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
        app_references=[orch.app_id],
        box_references=[
            au.BoxReference(app_id=0, name=b"__self_bytes"),
            au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
        ],
    ), send_params=AUTO_POPULATE)

    post_info = algod.application_info(orch.app_id)
    post_approval = post_info["params"]["approval-program"]

    assert pre_approval == post_approval, "orch approval bytes drifted across the dance"


def test_dance_call_getCollateral(split_exchange_with_delegate, localnet, universal_mock):
    """dance_call: install F → call orch.getCollateral() → revert.

    Proves the full dispatch path through the dance:
      - install loads helper2's bytes onto orch
      - calling orch with getCollateral's selector hits helper2's router
      - helper2's getCollateral reads orch's storage (collateral slot)
      - revert puts orch back to its original program

    The dance returns step-2's last_log so we can verify helper2 read
    the same collateral address that orch's __postInit wrote (which is
    the universal_mock app's address).
    """
    from algosdk.logic import get_application_address
    _, _, orch, chunk = split_exchange_with_delegate
    algod = localnet.client.algod

    pre = algod.application_info(orch.app_id)["params"]["approval-program"]

    res = chunk.send.call(au.AppClientMethodCallParams(
        method="dance_call",
        args=[
            bytes.fromhex("dc5e3798"),  # __delegate_update selector
            bytes.fromhex("dd1ce903"),  # getCollateral()address selector
        ],
        extra_fee=au.AlgoAmount(micro_algo=300_000),
        app_references=[orch.app_id],
        box_references=[
            au.BoxReference(app_id=0, name=b"__self_bytes"),
            au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
        ],
    ), send_params=AUTO_POPULATE)

    post = algod.application_info(orch.app_id)["params"]["approval-program"]
    assert pre == post, "orch approval bytes drifted across the dance"

    # dance_call returns step-2's last_log. The conftest packs each
    # "address" as 24 zero bytes + 8-byte big-endian app id (it's how
    # the Solidity address fields are stuffed for the test mock), so
    # the slot's 32-byte value is just the universal_mock app id at the
    # bottom. helper2's getCollateral returns that slot.
    ret_bytes = bytes(res.abi_return)
    # ABI marker (0x151f7c75) + length-prefixed dynamic-bytes for the
    # 32-byte slot. The slot's last 8 bytes carry the app id.
    last_8 = ret_bytes[-8:]
    got_app_id = int.from_bytes(last_8, "big")
    assert got_app_id == universal_mock.app_id, (
        f"getCollateral returned app id {got_app_id} (full ret = "
        f"{ret_bytes.hex()}), expected {universal_mock.app_id}")
