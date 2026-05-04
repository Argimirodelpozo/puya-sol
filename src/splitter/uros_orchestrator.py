"""puya-sol --uros-splitter orchestrator template (multi-chunk).

This algopy contract is the runtime piece of `--uros-splitter`. It
holds the main contract's bytecode plus N chunks of bytecode in box
storage, and per call swaps the right chunk into main, runs the
user's method, then swaps main back.

## Layout

State:
    main_app_id          UInt64    pinned at deploy time via set_main()
    main_bytes_len       UInt64    length of main's approval program
    chunk_count          UInt64    N — total registered chunks

Boxes:
    __codebox_main                 main contract's approval bytes
    __codebox_chunk_<i>            chunk i's approval bytes
    chunk_lens                     BoxMap[UInt64 chunk_idx → UInt64 length]
    chunk_for_selector             BoxMap[Bytes selector → UInt64 chunk_idx]

Boxes are allocated via setup_main_box / setup_chunk_box, then their
bytecode is streamed in via write_main / write_chunk (per-call write
budget caps a single chunk transfer at ~4 KB, so chunked uploads).
Each split method's selector is registered against its owning chunk
via register_chunk_method.

## Dance flow

User submits group [stub_call, dispatch_call]:

  txn 0 — ApplicationCall to main with (selector, args). Main's stub
          asserts that gtxn[group_index+1] is a dispatch() call to
          THIS orchestrator (orc-guard); the args land at gtxn[0]
          for the orch to forward.
  txn 1 — dispatch() on orch. The orch:
          1. reads selector = gtxn[group_index-1].ApplicationArgs[0]
          2. looks up chunk_idx = chunk_for_selector[selector]
          3. itxn 1: UpdateApplication main with chunk's bytes
          4. itxn 2: NoOp call main with the user's selector + args
                    (main is now running chunk's program)
          5. itxn 3: UpdateApplication main with main's original bytes

The 3 inner txns are atomically grouped — either all succeed or all
revert, leaving main's program unchanged on revert.

NOTE: there is intentionally NO __delegate_update on the orch. The
splitter adds __delegate_update to main + each chunk; the orch's
program is never updated as part of the dance.
"""

from algopy import (
    ARC4Contract,
    BoxMap,
    Bytes,
    OnCompleteAction,
    UInt64,
    arc4,
    itxn,
    op,
)


# Trivial clear program for the swap's clear-state slot.
# 0a = pragma version 10 (must match puya-sol's emission); 81 01 = pushint 1; 43 = return.
CLEAR_PROGRAM = b"\x0a\x81\x01\x43"

# `__delegate_update()void` ABI selector — sha512_256 of the sig, first 4 bytes.
# Both main and each chunk define this method; UpdateApplication itxns target
# this selector so the dance's swap-in/swap-out land cleanly.
DELEGATE_UPDATE_SELECTOR = b"\xdc\x5e\x37\x98"


class UrosOrchestrator(ARC4Contract):
    """Per-call program-swap orchestrator. Holds main + N chunks of
    bytecode in boxes; dispatch() runs the 3-itxn dance per call."""

    def __init__(self) -> None:
        self.main_app_id = UInt64(0)
        self.main_bytes_len = UInt64(0)
        self.chunk_count = UInt64(0)
        # chunk_idx → length of chunk's approval bytes
        self.chunk_lens = BoxMap(UInt64, UInt64, key_prefix=b"clen_")
        # selector (bytes4) → chunk_idx that holds the matching method
        self.chunk_for_selector = BoxMap(Bytes, UInt64, key_prefix=b"csel_")

    @arc4.abimethod(create="require")
    def init(self) -> None:
        """One-time AppCreate anchor. Other state is populated via
        the methods below."""

    @arc4.abimethod
    def set_main(self, main_app_id: UInt64) -> None:
        """Pin which application this orchestrator manages."""
        self.main_app_id = main_app_id

    @arc4.abimethod
    def setup_main_box(self, main_bytes_len: UInt64) -> None:
        """Allocate `__codebox_main` sized for the main approval bytes.
        Caller funds box MBR before invoking."""
        self.main_bytes_len = main_bytes_len
        _ok = op.Box.create(Bytes(b"__codebox_main"), main_bytes_len)
        assert _ok, "main codebox already exists"

    @arc4.abimethod
    def setup_chunk_box(self, chunk_idx: UInt64, chunk_bytes_len: UInt64) -> None:
        """Allocate `__codebox_chunk_<i>` for chunk_idx, record its
        length. Bumps chunk_count when a previously-unallocated index
        is added."""
        if chunk_idx + UInt64(1) > self.chunk_count:
            self.chunk_count = chunk_idx + UInt64(1)
        self.chunk_lens[chunk_idx] = chunk_bytes_len
        _ok = op.Box.create(
            Bytes(b"__codebox_chunk_") + op.itob(chunk_idx),
            chunk_bytes_len,
        )
        assert _ok, "chunk codebox already exists"

    @arc4.abimethod
    def write_main(self, offset: UInt64, data: Bytes) -> None:
        """Stream main approval bytes into __codebox_main. AVM caps
        per-call ApplicationArgs total at 2048 B, so callers chunk the
        bytecode and call this method until the box is fully populated."""
        op.Box.replace(Bytes(b"__codebox_main"), offset, data)

    @arc4.abimethod
    def write_chunk(self, chunk_idx: UInt64, offset: UInt64, data: Bytes) -> None:
        """Stream chunk approval bytes into __codebox_chunk_<idx>."""
        op.Box.replace(
            Bytes(b"__codebox_chunk_") + op.itob(chunk_idx),
            offset,
            data,
        )

    @arc4.abimethod
    def register_chunk_method(self, selector: Bytes, chunk_idx: UInt64) -> None:
        """Map an ABI selector to the chunk that holds its real body.
        Called once per split method at deploy time."""
        self.chunk_for_selector[selector] = chunk_idx

    @arc4.abimethod
    def dispatch(self) -> Bytes:
        """The dance: install chunk → run selector → restore main.
        Returns the inner call's last_log so the caller can decode
        the actual return value."""

        # Step 0: locate the user's stub call in the previous group txn.
        assert op.Txn.group_index > UInt64(0), "uros: dispatch needs a prev stub txn"
        prev_idx = op.Txn.group_index - UInt64(1)

        # Symmetric guard: prev txn must be an ApplicationCall to main.
        assert op.GTxn.type_enum(prev_idx) == op.GTxn.type_enum(op.Txn.group_index), \
            "uros: prev txn not appl"
        assert op.GTxn.application_id(prev_idx).id == self.main_app_id, \
            "uros: prev txn not main"

        # Look up which chunk holds the requested method.
        selector = op.GTxn.application_args(prev_idx, UInt64(0))
        chunk_idx = self.chunk_for_selector[selector]
        chunk_len = self.chunk_lens[chunk_idx]
        chunk_box = Bytes(b"__codebox_chunk_") + op.itob(chunk_idx)
        main_box = Bytes(b"__codebox_main")
        clear = Bytes(CLEAR_PROGRAM)
        page = UInt64(2048)

        # ── Step 1: install chunk's bytes on main ──────────────────
        # Programs are split into ≤4 pages of 2 KB each (AVM hard cap).
        # Branch on size to use the right page count for itxn submit.
        if chunk_len <= page:
            c0 = op.Box.extract(chunk_box, UInt64(0), chunk_len)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=c0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif chunk_len <= page * UInt64(2):
            c0 = op.Box.extract(chunk_box, UInt64(0), page)
            c1 = op.Box.extract(chunk_box, page, chunk_len - page)
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(c0, c1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif chunk_len <= page * UInt64(3):
            c0 = op.Box.extract(chunk_box, UInt64(0), page)
            c1 = op.Box.extract(chunk_box, page, page)
            c2 = op.Box.extract(chunk_box, page * UInt64(2), chunk_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(c0, c1, c2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        else:
            c0 = op.Box.extract(chunk_box, UInt64(0), page)
            c1 = op.Box.extract(chunk_box, page, page)
            c2 = op.Box.extract(chunk_box, page * UInt64(2), page)
            c3 = op.Box.extract(chunk_box, page * UInt64(3), chunk_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(c0, c1, c2, c3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()

        # ── Step 2: forward the user's call ────────────────────────
        # Pull selector + each arg from gtxn[prev_idx].ApplicationArgs.
        # AVM caps app_args count at 16; we forward up to 5 positional
        # args (covers the common case). Methods with more args need a
        # broader switch — extending is mechanical.
        n_args = op.GTxn.num_app_args(prev_idx)
        if n_args == UInt64(1):
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(selector,),
                fee=0,
            ).submit()
        elif n_args == UInt64(2):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(selector, a1),
                fee=0,
            ).submit()
        elif n_args == UInt64(3):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            a2 = op.GTxn.application_args(prev_idx, UInt64(2))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(selector, a1, a2),
                fee=0,
            ).submit()
        elif n_args == UInt64(4):
            a1 = op.GTxn.application_args(prev_idx, UInt64(1))
            a2 = op.GTxn.application_args(prev_idx, UInt64(2))
            a3 = op.GTxn.application_args(prev_idx, UInt64(3))
            call_res = itxn.ApplicationCall(
                app_id=self.main_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(selector, a1, a2, a3),
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
                app_args=(selector, a1, a2, a3, a4),
                fee=0,
            ).submit()

        ret = call_res.last_log

        # ── Step 3: restore main bytes ─────────────────────────────
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
