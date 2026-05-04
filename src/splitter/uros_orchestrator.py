"""puya-sol --uros-splitter orchestrator template.

This file is the runtime piece of the `--uros-splitter` technique. It's
an algopy contract that holds two approval-program byte blobs in box
storage and runs a 3-itxn dance to swap one in, call a function, swap
the original back.

Usage flow at deploy time (handled by the test harness or user, NOT by
puya-sol itself — puya-sol only emits the .approval.bin files this
contract loads):

    1. Compile this template via puyapy → orchestrator.approval.bin.
       Pin the orchestrator's app id (sets up state/boxes).
    2. Deploy the main contract (compiled by puya-sol with split-method
       bodies stubbed). Note its app id.
    3. Set main's manager to the orchestrator's address (or wire main's
       UpdateApplication path to assert sender == orchestrator).
    4. Call orchestrator.set_main(main_app_id).
    5. Allocate boxes:
         orchestrator.setup_boxes(len(main_bytes), len(helper_bytes))
    6. Stream the bytecodes into boxes (chunks <= 2048 B per call):
         orchestrator.write_codebox(0, offset, chunk)  # main bytes
         orchestrator.write_codebox(1, offset, chunk)  # helper bytes

Runtime call flow:

    User submits group [stub, dispatch]:
      txn 0: ApplicationCall to main with (selector=funcA, args=...).
             main's funcA body is a stub — runs without effect, but the
             selector + args are now in gtxn[0].ApplicationArgs.
      txn 1: ApplicationCall to orchestrator.dispatch(). The dispatcher
             reads gtxn[group_index - 1].ApplicationArgs to recover the
             selector and args, then submits an inner-txn group:

                 itxn 1: UpdateApplication on main with helper bytes
                         (loaded from box __codebox_1).
                 itxn 2: NoOp ApplicationCall on main with the same
                         selector + args. Helper code runs against
                         main's storage, produces real result.
                 itxn 3: UpdateApplication on main with main bytes
                         (restore from box __codebox_0).

             dispatch() returns itxn 2's last_log so the caller can
             decode the real result.

The 3 inner-txns are atomically grouped — either all succeed or all
revert, so main's approval is never left in the helper state.

Constants:
    DELEGATE_UPDATE_SELECTOR — sha512_256("__delegate_update()void")[:4],
    main's UpdateApplication-admitting branch matches on this selector.

Box layout: each box is sized at setup time to hold its full payload.
Reads use op.Box.extract chunked at 2048 B (AVM stack-value cap of 4096
plus headroom for length args). For payloads > 4096 we slice into a
tuple of chunks and pass to itxn.ApplicationCall's approval_program
field, which puyapy serialises as concatenated pages.
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

# Trivial clear program for the swap's clear-state slot. Hex layout:
#   0a   pragma version 10  (must match the program being swapped in)
#   81 01  pushint 1
#   43   return
# AVM enforces approval/clear major-version match. puya-sol targets AVM v10
# by default, so this hardcoded blob matches main+helper. If you bump
# puya-sol's target version, update this byte too (or, better: regenerate
# main's clear into a third box and read it dynamically).
CLEAR_PROGRAM = b"\x0a\x81\x01\x43"

# `__delegate_update()void` ABI selector — main's auto-generated router
# matches on this when OnCompletion=UpdateApplication, admitting only
# the swap from the orchestrator.
DELEGATE_UPDATE_SELECTOR = b"\xdc\x5e\x37\x98"


class UrosOrchestrator(ARC4Contract):
    """Per-call approval-program swap orchestrator (--uros-splitter)."""

    def __init__(self) -> None:
        self.main_app_id = UInt64(0)
        self.main_bytes_len = UInt64(0)
        self.helper_bytes_len = UInt64(0)

    @arc4.abimethod(create="require")
    def init(self) -> None:
        """One-time init at AppCreate. Boxes/lengths/main_app_id are
        populated by separate ABI calls below — this just exists to
        anchor the create transaction."""

    @arc4.abimethod
    def set_main(self, main_app_id: UInt64) -> None:
        """Pin the main contract's app id. Called after main is deployed."""
        self.main_app_id = main_app_id

    @arc4.abimethod
    def setup_boxes(
        self,
        main_bytes_len: UInt64,
        helper_bytes_len: UInt64,
    ) -> None:
        """Allocate __codebox_0 / __codebox_1 sized to fit the two
        bytecode payloads. Caller funds MBR before invoking."""
        self.main_bytes_len = main_bytes_len
        self.helper_bytes_len = helper_bytes_len
        # op.Box.create returns a bool ("did this allocation create a new
        # box?"); we don't care about the return value at setup time, but
        # binding it silences puyapy's "expression result is ignored" lint.
        _created_main = op.Box.create(Bytes(b"__codebox_0"), main_bytes_len)
        _created_helper = op.Box.create(Bytes(b"__codebox_1"), helper_bytes_len)
        assert _created_main and _created_helper, "boxes already exist"

    @arc4.abimethod
    def write_codebox(
        self,
        which: UInt64,
        offset: UInt64,
        data: Bytes,
    ) -> None:
        """Stream bytecode into __codebox_<which> at byte `offset`. The
        AVM caps single-call ApplicationArgs total at 2048 B, so callers
        chunk the bytecode and call this method repeatedly until both
        boxes are populated."""
        if which == UInt64(0):
            op.Box.replace(Bytes(b"__codebox_0"), offset, data)
        else:
            op.Box.replace(Bytes(b"__codebox_1"), offset, data)

    @arc4.abimethod
    def pad(self, which: UInt64) -> None:
        """No-op padding method that touches __codebox_<which>. Used by
        deploy harnesses to pool box read/write budget across a group
        of app-calls — each ApplicationCall txn that references a box
        and EXECUTES a box op contributes its 2048/4096 budgets to the
        group pool. We do a 1-byte box_extract here so the budget is
        actually contributed."""
        if which == UInt64(0):
            _byte = op.Box.extract(Bytes(b"__codebox_0"), UInt64(0), UInt64(1))
        else:
            _byte = op.Box.extract(Bytes(b"__codebox_1"), UInt64(0), UInt64(1))
        assert _byte.length == UInt64(1)

    @arc4.abimethod(allow_actions=("UpdateApplication",))
    def __delegate_update(self) -> None:
        """No-op admit branch for the orchestrator's own approval-program
        swaps. The dance below uses this selector to mark a swap as
        coming from the orchestrator. Mirror lives on main too — main's
        __delegate_update accepts the swap iff txn.Sender is this
        orchestrator's address."""

    @arc4.abimethod
    def dispatch(self) -> Bytes:
        """Read the previous group txn's ApplicationArgs (selector + args),
        submit the 3-itxn dance, return the dispatched call's last_log."""

        # Previous transaction holds the user-facing call to main's stub.
        # Its ApplicationArgs[0] is the ABI selector; [1..] are the
        # ABI-encoded positional args. We pass them through unchanged
        # to the inner call in step 2.
        prev_idx = op.Txn.group_index - UInt64(1)
        # We don't need to assert prev_idx was a call to main — if it
        # wasn't, step 2's selector dispatch will fail and the whole
        # dance reverts, leaving main's program unchanged.

        # Programs are split into N <= 4 pages of 2048 B each (AVM hard
        # cap is 4 pages = 8 KB total, and each page must fit in a stack
        # value capped at 4096 B). The branches below cover all 4 page
        # counts; itxn.ApplicationCall accepts a tuple of byte values
        # for approval_program, which puya serialises as concatenated
        # pages.
        main_box = Bytes(b"__codebox_0")
        helper_box = Bytes(b"__codebox_1")
        clear = Bytes(CLEAR_PROGRAM)
        page = UInt64(2048)

        # Step 1: install helper bytes on main.
        helper_len = self.helper_bytes_len
        if helper_len <= page:
            h0 = op.Box.extract(helper_box, UInt64(0), helper_len)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=h0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif helper_len <= page * UInt64(2):
            h0 = op.Box.extract(helper_box, UInt64(0), page)
            h1 = op.Box.extract(helper_box, page, helper_len - page)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(h0, h1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif helper_len <= page * UInt64(3):
            h0 = op.Box.extract(helper_box, UInt64(0), page)
            h1 = op.Box.extract(helper_box, page, page)
            h2 = op.Box.extract(helper_box, page * UInt64(2), helper_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(h0, h1, h2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        else:
            h0 = op.Box.extract(helper_box, UInt64(0), page)
            h1 = op.Box.extract(helper_box, page, page)
            h2 = op.Box.extract(helper_box, page * UInt64(2), page)
            h3 = op.Box.extract(helper_box, page * UInt64(3), helper_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(h0, h1, h2, h3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()

        # Step 2: forward the user's call. Pull selector + each arg from
        # gtxn[prev_idx].ApplicationArgs. AVM caps app_args count at 16,
        # so we forward up to that limit. Args beyond the actual count
        # will be empty bytes — accepted by puya's router but not used.
        sel = op.GTxn.application_args(prev_idx, UInt64(0))
        # NOTE: the static dispatch surface here is intentionally small.
        # Solidity contracts with > 4 args per split method need a
        # broader switch; extending this is mechanical (more cases).
        n_args = op.GTxn.num_app_args(prev_idx)
        # Forward fixed up to 4 positional args (covers the common case).
        # If a method needs more, regenerate this orchestrator with a
        # higher arity ceiling.
        if n_args == UInt64(1):
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel,),
                fee=0,
            ).submit()
        elif n_args == UInt64(2):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel, a1),
                fee=0,
            ).submit()
        elif n_args == UInt64(3):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            a2 = op.GTxn.application_args(prev_idx, UInt64(2))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel, a1, a2),
                fee=0,
            ).submit()
        elif n_args == UInt64(4):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            a2 = op.GTxn.application_args(prev_idx, UInt64(2))
            a3 = op.GTxn.application_args(prev_idx, UInt64(3))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel, a1, a2, a3),
                fee=0,
            ).submit()
        else:
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            a2 = op.GTxn.application_args(prev_idx, UInt64(2))
            a3 = op.GTxn.application_args(prev_idx, UInt64(3))
            a4 = op.GTxn.application_args(prev_idx, UInt64(4))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel, a1, a2, a3, a4),
                fee=0,
            ).submit()

        ret = call_res.last_log

        # Step 3: restore main bytes. Same 4-page branching as step 1.
        main_len = self.main_bytes_len
        if main_len <= page:
            m0 = op.Box.extract(main_box, UInt64(0), main_len)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=m0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif main_len <= page * UInt64(2):
            m0 = op.Box.extract(main_box, UInt64(0), page)
            m1 = op.Box.extract(main_box, page, main_len - page)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(m0, m1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif main_len <= page * UInt64(3):
            m0 = op.Box.extract(main_box, UInt64(0), page)
            m1 = op.Box.extract(main_box, page, page)
            m2 = op.Box.extract(main_box, page * UInt64(2), main_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(m0, m1, m2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        else:
            m0 = op.Box.extract(main_box, UInt64(0), page)
            m1 = op.Box.extract(main_box, page, page)
            m2 = op.Box.extract(main_box, page * UInt64(2), page)
            m3 = op.Box.extract(main_box, page * UInt64(3), main_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(m0, m1, m2, m3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()

        return ret
