"""Lonely-chunk sidecar for one delegated function F on the orchestrator.

Two contracts cooperate at runtime: this lonely chunk (orchestrates the
ceremony), and F-helper (puya-sol-emitted helper carrying the migrated
function body + its transitive closure). The lonely chunk is small (~500
bytes); F-helper is whatever the closure produces (e.g. matchOrders is
~7.7KB).

Dance:

  1. Install: UpdateApplication on orch with F-helper's bytes (read out
     of __self_bytes box — name is historical; box now holds F-helper's
     program, not this contract's). After this, orch's approval IS the
     F-helper program; orch's storage is unchanged.

  2. Call: ApplicationCall on orch with F's selector. orch is running
     F-helper's approval, so its router dispatches to F's body. F reads/
     writes orch's storage with the same keys it would use in orch's
     original program (since both come from the same Solidity source —
     puya-sol's storage codegen is deterministic on slot id).

  3. Revert: UpdateApplication on orch with orch's original bytes. F-
     helper has its own __delegate_update method (mirrored from orch) so
     this UpdateApplication call lands cleanly.

All three inner-txns must atomically succeed or the whole dance reverts
and orch's program is unchanged.

State + boxes (one-time populated at deploy by the test harness):
  orch_app_id          UInt64 — the orchestrator we update
  self_bytes_len       UInt64 — actual length of F-helper's program
  orch_orig_bytes_len  UInt64 — actual length of orch's original program
  __self_bytes         box, sized to fit f-helper bytes
  __orch_orig_bytes    box, sized to fit orch_orig_bytes_len
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
    def dance_call(
        self,
        delegate_update_selector: Bytes,
        target_selector: Bytes,
    ) -> Bytes:
        """Dance with a no-args step-2 call. Installs F onto orch, calls
        orch with `target_selector` (no extra app_args), reverts. Used
        for readonly probes like getCollateral() to verify the dispatch
        + storage-read path works through the dance. Returns step-2's
        last_log so the caller can decode the inner-call's return.
        """
        orch_id = self.orch_app_id
        self_box = Bytes(b"__self_bytes")
        orig_box = Bytes(b"__orch_orig_bytes")
        clear = Bytes(CLEAR_PROGRAM)

        self_p0 = op.Box.extract(self_box, UInt64(0), UInt64(2048))
        self_p1 = op.Box.extract(self_box, UInt64(2048), UInt64(2048))
        self_p2 = op.Box.extract(self_box, UInt64(4096), UInt64(2048))
        self_p3 = op.Box.extract(
            self_box,
            UInt64(6144),
            self.self_bytes_len - UInt64(6144),
        )
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(self_p0, self_p1, self_p2, self_p3),
            clear_state_program=clear,
            app_args=(delegate_update_selector,),
        ).submit()

        # Step 2: call orch (running F-helper bytes) with the target
        # selector. F-helper's router dispatches to the migrated method
        # body, which reads/writes orch's storage with the same keys
        # as orch's original program.
        call_res = itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.NoOp,
            app_args=(target_selector,),
        ).submit()
        ret = call_res.last_log

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
        return ret

    @arc4.abimethod
    def delegate_dance(self, delegate_update_selector: Bytes) -> None:
        """Install F-helper bytes onto orch, call orch (now running F),
        revert. The 3 inner-txns must atomically succeed or the whole
        dance reverts and orch's program is unchanged.

        AVM stack values cap at 4096 bytes, so F-helper >4KB has to be
        installed via a tuple of pages. We slice into 4 × 2048-byte
        pages, with the last page sized to the tail. F-helper is
        expected to be 4096 < len < 8192 (single program max).
        """
        orch_id = self.orch_app_id
        self_box = Bytes(b"__self_bytes")
        orig_box = Bytes(b"__orch_orig_bytes")
        clear = Bytes(CLEAR_PROGRAM)

        # 1. install F-helper → orch. Read 4 pages of 2048 bytes each.
        # The last page's slice is sized to the tail so we don't push
        # trailing junk. ApplicationArgs[0] is __delegate_update's
        # selector so orch's router takes the UpdateApplication branch.
        self_p0 = op.Box.extract(self_box, UInt64(0), UInt64(2048))
        self_p1 = op.Box.extract(self_box, UInt64(2048), UInt64(2048))
        self_p2 = op.Box.extract(self_box, UInt64(4096), UInt64(2048))
        self_p3 = op.Box.extract(
            self_box,
            UInt64(6144),
            self.self_bytes_len - UInt64(6144),
        )
        itxn.ApplicationCall(
            app_id=orch_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(self_p0, self_p1, self_p2, self_p3),
            clear_state_program=clear,
            app_args=(delegate_update_selector,),
        ).submit()

        # 2. call orch (now running F-helper). Args forwarding left
        # blank for now — for the plumbing round-trip test we just need
        # install/revert to succeed; matchOrders args are added in a
        # follow-up via a per-F dance variant.

        # 3. revert orch → original. orch's original is 4096 < len <
        # 6144 (v2 split orch is ~5.3KB with __delegate_update), so 3
        # pages: two 2KB pages plus a tail.
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
