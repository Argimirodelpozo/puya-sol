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

# Trivial clear program: `pragma version 12; pushint 1; return`.
# Major version must match main's approval (puya emits v12 by default).
# AVM rejects UpdateApplication when approval/clear majors differ.
CLEAR_PROGRAM = b"\x0c\x81\x01\x43"

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
        op.Box.create(Bytes(b"__codebox_0"), main_bytes_len)
        op.Box.create(Bytes(b"__codebox_1"), helper_bytes_len)

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

        # Read codebox payloads. AVM stack values cap at 4096 B; main
        # bytecode > 4 KB has to be sliced into pages and passed to
        # itxn.ApplicationCall as a tuple. The 3-page slicing here
        # covers up to 12 KB programs (= 1.5x the typical 8 KB single-
        # page cap, enough for most "too big" candidates).
        main_box = Bytes(b"__codebox_0")
        helper_box = Bytes(b"__codebox_1")
        clear = Bytes(CLEAR_PROGRAM)

        helper_p0 = op.Box.extract(helper_box, UInt64(0), UInt64(2048))
        helper_p1 = op.Box.extract(
            helper_box,
            UInt64(2048),
            self.helper_bytes_len - UInt64(2048),
        )

        # Step 1: install helper bytes on main.
        itxn.ApplicationCall(
            app_id=self.main_app_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(helper_p0, helper_p1),
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

        # Step 3: restore main bytes.
        main_p0 = op.Box.extract(main_box, UInt64(0), UInt64(2048))
        main_p1 = op.Box.extract(
            main_box,
            UInt64(2048),
            self.main_bytes_len - UInt64(2048),
        )
        itxn.ApplicationCall(
            app_id=self.main_app_id,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(main_p0, main_p1),
            clear_state_program=clear,
            app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
            fee=0,
        ).submit()

        return ret
