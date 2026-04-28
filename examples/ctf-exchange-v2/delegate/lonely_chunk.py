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

Boxes (one-time populated at deploy by the test harness via setSelfBytes
/ setOrchOrigBytes):
  __self_bytes      this contract's own compiled approval bytes
                    (4 × 2KB pages, written by deploy harness)
  __orch_orig_bytes orch's original approval bytes (same form)

State:
  orch_app_id       UInt64 — the orchestrator we're updating, set in
                    the constructor at deploy time.
"""
from algopy import (
    ARC4Contract,
    Box,
    Bytes,
    OnCompleteAction,
    UInt64,
    arc4,
    itxn,
    op,
)


# 1-byte clear program ("pragma version 9; intc_0; return"). Compiled
# bytes; literal here so we don't have to read it from box.
CLEAR_PROGRAM = b"\x09\x81\x01\x43"

# Each box holds 4 × 2KB pages = 8192 bytes (the AVM approval-program cap).
# Inlined throughout because puya-Python disallows module-level computed
# constants of UInt64 type.


class LonelyChunk(ARC4Contract):
    """Sidecar for one delegated function on the orchestrator."""

    def __init__(self) -> None:
        # State (uint64). Set by the create call.
        self.orch_app_id = UInt64(0)

    @arc4.abimethod(create="require")
    def init(self, orch_app_id: UInt64) -> None:
        """Pin which orchestrator app we update. Called once at deploy.
        Box allocation happens in setup() AFTER the app is funded — boxes
        require MBR proportional to size, and the app's account doesn't
        exist before create."""
        self.orch_app_id = orch_app_id

    @arc4.abimethod
    def setup_boxes(self) -> None:
        """Allocate __self_bytes / __orch_orig_bytes boxes (8KB each).
        Caller must have funded the app's account with enough MBR
        (~6.7 ALGO) before this is invoked."""
        op.Box.create(Bytes(b"__self_bytes"), UInt64(8192))
        op.Box.create(Bytes(b"__orch_orig_bytes"), UInt64(8192))

    @arc4.abimethod
    def set_self_chunk(self, offset: UInt64, data: Bytes) -> None:
        """Write a chunk of this contract's own approval bytes into
        __self_bytes at the given byte offset. Tests call this in 1KB
        slices (the per-call ApplicationArgs total cap is 2048)."""
        op.Box.replace(Bytes(b"__self_bytes"), offset, data)

    @arc4.abimethod
    def set_orch_orig_chunk(self, offset: UInt64, data: Bytes) -> None:
        """Write a chunk of orch's original approval bytes into
        __orch_orig_bytes at the given byte offset."""
        op.Box.replace(Bytes(b"__orch_orig_bytes"), offset, data)

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

        # 2. call orch (now running self's approval). Args forwarding is
        # left blank for now; once matchOrders is in this contract the
        # caller would forward args[1..N] explicitly via app_args=(...).

        # 3. revert orch → original
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(orig_p0, orig_p1, orig_p2, orig_p3),
            clear_state_program=clear,
        ).submit()
