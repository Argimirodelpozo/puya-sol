"""puya-sol --uros-splitter orchestrator template.

The runtime piece of `--uros-splitter`. Holds N chunks of bytecode in
boxes; per call swaps the right chunk onto __storage, runs the user's
method, then swaps __storage back to its admit-update default.

## Architecture (3 contracts)

  user / Spoke      ─call→  main (stubs)
                              │ inner-call, app_args = forwarded
                              ▼
                            orch.dispatch()  ◄── this contract
                              │ 3 inner txns
                              ▼
                            __storage  (state holder)

main forwards user's [selector, *args] into orch.dispatch's
ApplicationArgs[1..]. orch reads the selector at [1] (its own [0] is
the dispatch selector), looks up which chunk holds that method,
installs the chunk on __storage via UpdateApplication, calls
__storage with [selector, *args], then restores __storage's default
bytecode. Returns the inner call's last_log so main can forward it
to the original caller.

Inner-txn callers work transparently because main is just a regular
contract — Spoke→main.foo is a normal inner txn, main's stub then
inner-calls orch, which inner-calls __storage. Max depth ≈ 4.

## Layout

State:
    storage_app_id       UInt64    set via set_storage(); the app
                                   chunks run on (state holder)
    storage_default_len  UInt64    length of __storage's default
                                   bytecode (the admit-update one)
    chunk_count          UInt64    N — total registered chunks

Boxes:
    __codebox_default              __storage's default approval bytes
                                   (used to restore after each call)
    __codebox_chunk_<i>            chunk i's approval bytes
    chunk_lens                     BoxMap[UInt64 idx → UInt64 length]
    chunk_for_selector             BoxMap[Bytes selector → UInt64 idx]
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


# Trivial clear program (must match puya-sol's emission).
CLEAR_PROGRAM = b"\x0a\x81\x01\x43"

# `__delegate_update()void` ABI selector — sha512_256(sig)[:4]. Both
# the __storage default and the chunks define this method; orch sends
# UpdateApplication itxns targeting this selector.
DELEGATE_UPDATE_SELECTOR = b"\xdc\x5e\x37\x98"


class UrosOrchestrator(ARC4Contract):
    """Per-call program-swap orchestrator. Holds N chunks of bytecode
    in boxes; dispatch() runs the 3-itxn dance per call against
    __storage."""

    def __init__(self) -> None:
        self.storage_app_id = UInt64(0)
        self.storage_default_len = UInt64(0)
        self.chunk_count = UInt64(0)
        self.chunk_lens = BoxMap(UInt64, UInt64, key_prefix=b"clen_")
        self.chunk_for_selector = BoxMap(Bytes, UInt64, key_prefix=b"csel_")

    @arc4.abimethod(create="require")
    def init(self) -> None:
        """One-time AppCreate anchor."""

    @arc4.abimethod
    def set_storage(self, storage_app_id: UInt64) -> None:
        """Pin which __storage app this orch manages."""
        self.storage_app_id = storage_app_id

    @arc4.abimethod
    def setup_default_box(self, default_bytes_len: UInt64) -> None:
        """Allocate `__codebox_default` for __storage's default
        bytecode. Caller funds box MBR before invoking."""
        self.storage_default_len = default_bytes_len
        _ok = op.Box.create(Bytes(b"__codebox_default"), default_bytes_len)
        assert _ok, "default codebox already exists"

    @arc4.abimethod
    def setup_chunk_box(self, chunk_idx: UInt64, chunk_bytes_len: UInt64) -> None:
        """Allocate `__codebox_chunk_<i>` for chunk_idx, record its
        length. Bumps chunk_count when a new index is added."""
        if chunk_idx + UInt64(1) > self.chunk_count:
            self.chunk_count = chunk_idx + UInt64(1)
        self.chunk_lens[chunk_idx] = chunk_bytes_len
        _ok = op.Box.create(
            Bytes(b"__codebox_chunk_") + op.itob(chunk_idx),
            chunk_bytes_len,
        )
        assert _ok, "chunk codebox already exists"

    @arc4.abimethod
    def write_default(self, offset: UInt64, data: Bytes) -> None:
        """Stream __storage's default approval bytes into __codebox_default."""
        op.Box.replace(Bytes(b"__codebox_default"), offset, data)

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
        """Map an ABI selector to its owning chunk."""
        self.chunk_for_selector[selector] = chunk_idx

    @arc4.abimethod
    def dispatch(self) -> Bytes:
        """The dance: install chunk → run selector on __storage →
        restore default. Returns the inner call's last_log.

        ApplicationArgs layout (from main's forwarded inner txn):
          [0] = "dispatch()" selector  (consumed by ABI router)
          [1] = user's selector  (the original method)
          [2..N+1] = user's args (forwarded as-is)
        """

        # The user's selector arrived at our ApplicationArgs[1].
        user_selector = op.Txn.application_args(UInt64(1))
        chunk_idx = self.chunk_for_selector[user_selector]
        chunk_len = self.chunk_lens[chunk_idx]
        chunk_box = Bytes(b"__codebox_chunk_") + op.itob(chunk_idx)
        default_box = Bytes(b"__codebox_default")
        default_len = self.storage_default_len
        clear = Bytes(CLEAR_PROGRAM)
        page = UInt64(2048)
        target_app = self.storage_app_id

        # ── Step 1: install chunk on __storage ────────────────────────
        if chunk_len <= page:
            c0 = op.Box.extract(chunk_box, UInt64(0), chunk_len)
            itxn.ApplicationCall(
                app_id=target_app,
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
                app_id=target_app,
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
                app_id=target_app,
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
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(c0, c1, c2, c3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()

        # ── Step 2: forward the user's call to __storage ─────────────
        # ApplicationArgs to __storage = [user_selector, *user_args].
        # We read user_args from our own Txn.ApplicationArgs[2..N+1].
        # AVM caps app_args at 16 entries; static branch on
        # NumAppArgs - 1 (we exclude our own dispatch selector at [0]).
        n = op.Txn.num_app_args - UInt64(1)
        if n == UInt64(1):
            # Just selector, no extra args
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector,),
                fee=0,
            ).submit()
        elif n == UInt64(2):
            a1 = op.Txn.application_args(UInt64(2))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1),
                fee=0,
            ).submit()
        elif n == UInt64(3):
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1, a2),
                fee=0,
            ).submit()
        elif n == UInt64(4):
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            a3 = op.Txn.application_args(UInt64(4))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1, a2, a3),
                fee=0,
            ).submit()
        elif n == UInt64(5):
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            a3 = op.Txn.application_args(UInt64(4))
            a4 = op.Txn.application_args(UInt64(5))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1, a2, a3, a4),
                fee=0,
            ).submit()
        else:
            # 6+ args: use up to 6, AVM ApplicationArgs max is 16
            # (15 + selector). For more, callers pack into byte[] tuple.
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            a3 = op.Txn.application_args(UInt64(4))
            a4 = op.Txn.application_args(UInt64(5))
            a5 = op.Txn.application_args(UInt64(6))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1, a2, a3, a4, a5),
                fee=0,
            ).submit()

        ret = call_res.last_log

        # ── Step 3: restore __storage's default bytecode ──────────────
        if default_len <= page:
            d0 = op.Box.extract(default_box, UInt64(0), default_len)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=d0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif default_len <= page * UInt64(2):
            d0 = op.Box.extract(default_box, UInt64(0), page)
            d1 = op.Box.extract(default_box, page, default_len - page)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(d0, d1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        elif default_len <= page * UInt64(3):
            d0 = op.Box.extract(default_box, UInt64(0), page)
            d1 = op.Box.extract(default_box, page, page)
            d2 = op.Box.extract(default_box, page * UInt64(2), default_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(d0, d1, d2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()
        else:
            d0 = op.Box.extract(default_box, UInt64(0), page)
            d1 = op.Box.extract(default_box, page, page)
            d2 = op.Box.extract(default_box, page * UInt64(2), page)
            d3 = op.Box.extract(default_box, page * UInt64(3), default_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(d0, d1, d2, d3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).submit()

        return ret
