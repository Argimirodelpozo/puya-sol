"""Lonely-chunk sidecar for one delegated function on the orchestrator.

Single contract, dual-mode:

  1. When called normally on its own app id, `delegate_dance` fires three
     inner-txns: install (UpdateApplication on orch with this contract's
     bytes — read out of __self_bytes box), call (ApplicationCall on
     orch — orch is now running this contract's approval against orch's
     storage and dispatches to a migrated method body), revert
     (UpdateApplication back to original — read from __orch_orig_bytes
     box).

  2. When orch's approval has been swapped to this contract's bytes and
     called, the same program runs but the dispatcher routes to the
     migrated method body (e.g. matchOrders), which reads/writes orch's
     storage with the same keys it used in the original orchestrator.

The migrated bodies are appended to this contract by the splitter
(matchOrders + its closure of internal callees). For now this file
defines only the dance scaffolding; merging happens post-puya.

State + boxes (one-time populated at deploy by the test harness):
  orch_app_id          UInt64 — the orchestrator we update
  self_bytes_len       UInt64 — actual length of this contract's program
  orch_orig_bytes_len  UInt64 — actual length of orch's original program
  __self_bytes         box, sized to fit self_bytes_len
  __orch_orig_bytes    box, sized to fit orch_orig_bytes_len

Programs ≤8192 bytes each, but approval + clear combined must stay
≤8192. The dance reads the actual lengths from state and pushes only
the necessary pages, padding the last page with zeros to never write
trailing junk to orch's program.
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


# Trivial clear-state program: `pragma version 12; pushint 1; return`.
# Version byte must match orch's approval (puya-sol emits v12).
# AVM rejects UpdateApplication when approval/clear major versions
# differ.
CLEAR_PROGRAM = b"\x0c\x81\x01\x43"

# ARC-4 selector for orch's `__delegate_update()void` method (sha512_256
# of the signature, first 4 bytes). The orch's auto-generated router
# matches on this selector when OnCompletion=UpdateApplication.
DELEGATE_UPDATE_SELECTOR = b"\xdc\x5e\x37\x98"


class LonelyChunk(ARC4Contract):
    """Sidecar for one delegated function on the orchestrator."""

    def __init__(self) -> None:
        self.orch_app_id = UInt64(0)
        self.self_bytes_len = UInt64(0)
        self.orch_orig_bytes_len = UInt64(0)

    @arc4.abimethod(create="require")
    def init(
        self,
        orch_app_id: UInt64,
        self_bytes_len: UInt64,
        orch_orig_bytes_len: UInt64,
    ) -> None:
        """Pin which orchestrator app we update + the actual lengths of
        the two programs we'll be storing in boxes. Box allocation is
        deferred to setup_boxes() — boxes need MBR which the create
        txn's account hasn't received yet."""
        self.orch_app_id = orch_app_id
        self.self_bytes_len = self_bytes_len
        self.orch_orig_bytes_len = orch_orig_bytes_len

    @arc4.abimethod(allow_actions=("UpdateApplication",))
    def __delegate_update(self) -> None:
        """Mirror of orch's __delegate_update — admits the revert step
        of the dance. While orch's approval is the lonely chunk's bytes
        (mid-dance), the revert inner-txn calls orch with
        OC=UpdateApplication + this selector; without this branch the
        revert reverts the whole dance atomically."""

    @arc4.abimethod
    def setup_boxes(self) -> None:
        """Allocate __self_bytes / __orch_orig_bytes boxes sized to
        the lengths registered in init(). Each box's MBR must be funded
        by the caller before this is invoked."""
        op.Box.create(Bytes(b"__self_bytes"), self.self_bytes_len)
        op.Box.create(Bytes(b"__orch_orig_bytes"), self.orch_orig_bytes_len)

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
    def delegate_dance(self, delegate_update_selector: Bytes) -> None:
        """Install self bytes onto orch, call orch (which now runs as
        F), revert. The 3 inner-txns must atomically succeed or the
        whole dance reverts and orch's program is unchanged.

        Programs are pushed page-by-page using ApprovalProgramPages.
        We always emit 4 pages but extract the actual byte count for
        each, so trailing pages are empty when the source program is
        smaller than 8KB.
        """
        orch_id = self.orch_app_id
        self_box = Bytes(b"__self_bytes")
        orig_box = Bytes(b"__orch_orig_bytes")
        clear = Bytes(CLEAR_PROGRAM)

        # Read self bytes (single page, ≤8KB; we expect ≤4KB for the
        # lonely chunk shell + injected migrated body so this fits in
        # one ApprovalProgram field). For larger F's, splitter will
        # need to extend to per-page extraction.
        self_p0 = op.Box.extract(self_box, UInt64(0), self.self_bytes_len)

        # 1. install self → orch. ApplicationArgs[0] must be the
        # __delegate_update selector so the orch's router routes to its
        # UpdateApplication-allowed branch.
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=self_p0,
            clear_state_program=clear,
            app_args=(delegate_update_selector,),
        ).submit()

        # 2. call orch (now running self's approval). Args forwarding
        # left blank for now; once matchOrders is in this contract the
        # caller would forward args[1..N] explicitly via app_args=(...).

        # 3. revert orch → original. orch is bigger than the 4KB
        # ApprovalProgram cap, so split into 3 pages: two full 2KB
        # pages plus a tail of (len - 4096) bytes. Assumes
        # 4096 < orch_orig_bytes_len < 6144 (true for the v2 split orch
        # at 5266B with __delegate_update). For larger F's we'd need
        # to extend to a 4-page form.
        orig_p0 = op.Box.extract(orig_box, UInt64(0), UInt64(2048))
        orig_p1 = op.Box.extract(orig_box, UInt64(2048), UInt64(2048))
        orig_p2 = op.Box.extract(
            orig_box,
            UInt64(4096),
            self.orch_orig_bytes_len - UInt64(4096),
        )

        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(orig_p0, orig_p1, orig_p2),
            clear_state_program=clear,
            app_args=(delegate_update_selector,),
        ).submit()
