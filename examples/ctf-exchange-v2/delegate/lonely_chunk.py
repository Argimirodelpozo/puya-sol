"""Lonely-chunk sidecar for one delegated function on the orchestrator.

Single contract, dual-mode:

  1. When called normally on its own app id, `delegate_dance` fires three
     inner-txns: install (UpdateApplication on orch with this contract's
     bytes — read out of __self_bytes box in 4 × 2KB pages), call
     (ApplicationCall on orch — orch is now running this contract's
     approval against orch's storage and dispatches to a migrated
     method body), revert (UpdateApplication back to original — read
     from __orch_orig_bytes box).

  2. When orch's approval has been swapped to this contract's bytes and
     called, the same program runs but the dispatcher routes to the
     migrated method body (e.g. matchOrders), which reads/writes orch's
     storage with the same keys it used in the original orchestrator.

The migrated bodies are appended to this contract by the splitter
(matchOrders + its closure of internal callees). For now this file
defines only the dance scaffolding; merging happens post-puya.

Boxes (one-time populated at deploy):
  __self_bytes      this contract's own compiled approval bytes
                    (4 × 2KB pages, written by deploy harness)
  __orch_orig_bytes orch's original approval bytes (same form)

State:
  orch_app_id       UInt64 — the orchestrator we're updating, set in
                    the constructor at deploy time.
"""
from algopy import (
    ARC4Contract,
    Bytes,
    OnCompleteAction,
    UInt64,
    arc4,
    itxn,
    op,
)


# 1-byte clear program ("pragma version 9; intc_0; return"). Compiled
# bytes; literal here so we don't have to read it from box. The number
# of pragma version doesn't matter for clear-state semantics.
CLEAR_PROGRAM = b"\x09\x81\x01\x43"


class LonelyChunk(ARC4Contract):
    """Sidecar for one delegated function on the orchestrator."""

    def __init__(self) -> None:
        # State (uint64). Will be set by the create call.
        self.orch_app_id = UInt64(0)

    @arc4.abimethod(create="require")
    def init(self, orch_app_id: UInt64) -> None:
        """Pin which orchestrator app we update. Called once at deploy."""
        self.orch_app_id = orch_app_id

    @arc4.abimethod
    def delegate_dance(self) -> None:
        """Install self bytes onto orch, call orch (which now runs as
        F), revert. The 3 inner-txns must atomically succeed or the
        whole dance reverts and orch's program is unchanged.
        """
        orch_id = self.orch_app_id

        # Read this contract's own approval bytes from __self_bytes box,
        # in 4 × 2KB pages. box_extract returns ≤4KB stack values.
        self_box = Bytes(b"__self_bytes")
        self_p0 = op.Box.extract(self_box, UInt64(0), UInt64(2048))
        self_p1 = op.Box.extract(self_box, UInt64(2048), UInt64(2048))
        self_p2 = op.Box.extract(self_box, UInt64(4096), UInt64(2048))
        self_p3 = op.Box.extract(self_box, UInt64(6144), UInt64(2048))

        orig_box = Bytes(b"__orch_orig_bytes")
        orig_p0 = op.Box.extract(orig_box, UInt64(0), UInt64(2048))
        orig_p1 = op.Box.extract(orig_box, UInt64(2048), UInt64(2048))
        orig_p2 = op.Box.extract(orig_box, UInt64(4096), UInt64(2048))
        orig_p3 = op.Box.extract(orig_box, UInt64(6144), UInt64(2048))

        clear = Bytes(CLEAR_PROGRAM)

        # 1. install self → orch
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(self_p0, self_p1, self_p2, self_p3),
            clear_state_program=clear,
        ).submit()

        # 2. call orch (now running self's approval). The migrated
        # method's selector + args are forwarded from app_args[1..N];
        # we don't peek at them — the original args bytes are passed
        # through as-is.
        # NOTE: forwarding-args wiring deferred until splitter merges
        # the migrated method into this contract.

        # 3. revert orch → original
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(orig_p0, orig_p1, orig_p2, orig_p3),
            clear_state_program=clear,
        ).submit()
