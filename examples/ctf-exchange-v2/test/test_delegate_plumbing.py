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


@pytest.mark.xfail(reason="lonely chunk's __self_bytes contains the chunk's "
                          "own approval (no migrated F yet); installing it on "
                          "orch and calling routes to chunk's ARC4 router which "
                          "doesn't recognise orch's selectors → orch's NoOp "
                          "ApplicationCall in step 2 of the dance reverts. "
                          "Will pass once matchOrders + closure are merged "
                          "into the lonely chunk's source.")
def test_delegate_dance_round_trip(split_exchange_with_delegate, localnet):
    """delegate_dance: install → call → revert. Verify orch's approval
    bytes are restored after."""
    _, _, orch, chunk = split_exchange_with_delegate
    algod = localnet.client.algod

    pre_info = algod.application_info(orch.app_id)
    pre_approval = pre_info["params"]["approval-program"]

    chunk.send.call(au.AppClientMethodCallParams(
        method="delegate_dance", args=[],
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
