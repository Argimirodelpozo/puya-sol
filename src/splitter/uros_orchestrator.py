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
    Application,
    ARC4Contract,
    BoxMap,
    Bytes,
    OnCompleteAction,
    UInt64,
    arc4,
    itxn,
    op,
    subroutine,
)


# Trivial clear program (must match puya-sol's emission).
CLEAR_PROGRAM = b"\x0c\x81\x01\x43"  # v12 "int 1; return" — must match the
# user contract's approval program version (puya-sol emits v12). AVM
# rejects UpdateApplication when approval/clear versions differ.

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
        # Per-selector chain registration: packed sequence of 12-byte
        # entries, each `(chunk_idx_uint64_be, piece_selector_4bytes)`.
        # When dispatch is called for a registered chain, all pieces'
        # install+call cycles run as one staged inner-txn group so
        # successive pieces can `gload <prev_call_txn_idx> 100` to read
        # the scratch slot 100 written by the previous piece's epilogue
        # (FunctionSplitter cross-chunk live-vars threading).
        self.chain_for_selector = BoxMap(Bytes, Bytes, key_prefix=b"cchain_")

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
    def register_chunk_method_chain(self, selector: Bytes, entries: Bytes) -> None:
        """Register a piece-chain for `selector`. `entries` is a packed
        byte string of N×12-byte records: 8 bytes big-endian
        `chunk_idx`, then 4 bytes `piece_selector`. dispatch() detects
        this entry first and routes via dispatch_chain semantics so
        live vars cross piece boundaries via gload."""
        self.chain_for_selector[selector] = entries

    # ── Internal helper subroutines for dispatch_chain ──
    # Three helpers, one per stage shape. Each branches on its size
    # arg internally, so the body of dispatch_chain stays small.

    @subroutine
    def _stage_install_first(self, install_box: Bytes,
                              install_len: UInt64) -> None:
        """Stage the FIRST install in the chain (begin_group=True).
        Multi-page: chunks ≤ 1/2/3/4 pages packed as a tuple."""
        target_app = self.storage_app_id
        clear = Bytes(CLEAR_PROGRAM)
        page = UInt64(2048)
        if install_len <= page:
            p0 = op.Box.extract(install_box, UInt64(0), install_len)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=p0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage(begin_group=True)
        elif install_len <= page * UInt64(2):
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, install_len - page)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage(begin_group=True)
        elif install_len <= page * UInt64(3):
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, page)
            p2 = op.Box.extract(install_box, page * UInt64(2),
                                install_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1, p2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage(begin_group=True)
        else:
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, page)
            p2 = op.Box.extract(install_box, page * UInt64(2), page)
            p3 = op.Box.extract(install_box, page * UInt64(3),
                                install_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1, p2, p3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage(begin_group=True)

    @subroutine
    def _stage_install_next(self, install_box: Bytes,
                             install_len: UInt64) -> None:
        """Stage a non-first install (or the final restore). Same
        4-page branching as `_stage_install_first` but no
        begin_group=True. begin_group is a Python compile-time bool
        (algopy can't pass a runtime bool to .stage), so we have two
        sibling subroutines instead of one with a flag arg."""
        target_app = self.storage_app_id
        clear = Bytes(CLEAR_PROGRAM)
        page = UInt64(2048)
        if install_len <= page:
            p0 = op.Box.extract(install_box, UInt64(0), install_len)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=p0,
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage()
        elif install_len <= page * UInt64(2):
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, install_len - page)
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage()
        elif install_len <= page * UInt64(3):
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, page)
            p2 = op.Box.extract(install_box, page * UInt64(2),
                                install_len - page * UInt64(2))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1, p2),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage()
        else:
            p0 = op.Box.extract(install_box, UInt64(0), page)
            p1 = op.Box.extract(install_box, page, page)
            p2 = op.Box.extract(install_box, page * UInt64(2), page)
            p3 = op.Box.extract(install_box, page * UInt64(3),
                                install_len - page * UInt64(3))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.UpdateApplication,
                approval_program=(p0, p1, p2, p3),
                clear_state_program=clear,
                app_args=(Bytes(DELEGATE_UPDATE_SELECTOR),),
                fee=0,
            ).stage()

    @subroutine
    def _stage_call(self, piece_sel: Bytes) -> None:
        """Stage a piece's call with forwarded user args. Branches on
        n_user_args = num_app_args − 1 (1/2/3/up-to-5)."""
        target_app = self.storage_app_id
        main_app = Application(op.Global.caller_application_id)
        n_user_args = op.Txn.num_app_args - UInt64(1)
        if n_user_args == UInt64(1):
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(piece_sel,),
                apps=(main_app,),
                fee=0,
            ).stage()
        elif n_user_args == UInt64(2):
            a1 = op.Txn.application_args(UInt64(2))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(piece_sel, a1),
                apps=(main_app,),
                fee=0,
            ).stage()
        elif n_user_args == UInt64(3):
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(piece_sel, a1, a2),
                apps=(main_app,),
                fee=0,
            ).stage()
        else:
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            a3 = op.Txn.application_args(UInt64(4))
            a4 = op.Txn.application_args(UInt64(5))
            itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(piece_sel, a1, a2, a3, a4),
                apps=(main_app,),
                fee=0,
            ).stage()

    @arc4.abimethod
    def dispatch_chain(self) -> Bytes:
        """Chain-dispatch a registered piece chain. ApplicationArgs
        layout matches dispatch():
            [0] = "dispatch_chain()" selector
            [1] = user's primary selector (the original method)
            [2..] = user's args (forwarded to every piece)

        For chain_len N, stages 2N+1 inner-txns in one group:
            install_0 (begin), call_0, install_1, call_1, ...,
            install_{N-1}, call_{N-1}, restore (default).
        Returns op.GITxn.last_log(2N-1) — last piece's call output.

        v3 supports chain_len ∈ {2..7} (2N+1 ≤ 16 AVM inner-txn cap).
        Each piece's chunk and the default storage may be up to 4
        pages (8 KB).

        Implementation: install/call/restore are three helper
        subroutines. dispatch_chain decodes pieces, then orchestrates
        the per-chain-length sequence as a flat list of subroutine
        calls. Keeps the contract small (subroutine bodies emitted
        once, called many times).
        """
        user_selector = op.Txn.application_args(UInt64(1))
        entries = self.chain_for_selector[user_selector]
        # Each entry: 8 bytes chunk_idx + 4 bytes piece_selector = 12 B.
        chain_len = entries.length // UInt64(12)
        default_box = Bytes(b"__codebox_default")
        default_len = self.storage_default_len

        # Decode up to 7 piece entries. Reads past `chain_len` are
        # gated by the chain_len switch below — we never USE the
        # decoded values for pieces beyond chain_len, but the decode
        # must be statically reachable for algopy to typecheck.
        # Use op.extract bounded reads — entries may be < 7*12 bytes,
        # so we only decode within entries.length.

        if chain_len == UInt64(2):
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(3)
        elif chain_len == UInt64(3):
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            c2_idx = op.btoi(op.extract(entries, UInt64(24), UInt64(8)))
            c2_sel = op.extract(entries, UInt64(32), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c2_idx),
                self.chunk_lens[c2_idx])
            self._stage_call(c2_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(5)
        elif chain_len == UInt64(4):
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            c2_idx = op.btoi(op.extract(entries, UInt64(24), UInt64(8)))
            c2_sel = op.extract(entries, UInt64(32), UInt64(4))
            c3_idx = op.btoi(op.extract(entries, UInt64(36), UInt64(8)))
            c3_sel = op.extract(entries, UInt64(44), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c2_idx),
                self.chunk_lens[c2_idx])
            self._stage_call(c2_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c3_idx),
                self.chunk_lens[c3_idx])
            self._stage_call(c3_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(7)
        elif chain_len == UInt64(5):
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            c2_idx = op.btoi(op.extract(entries, UInt64(24), UInt64(8)))
            c2_sel = op.extract(entries, UInt64(32), UInt64(4))
            c3_idx = op.btoi(op.extract(entries, UInt64(36), UInt64(8)))
            c3_sel = op.extract(entries, UInt64(44), UInt64(4))
            c4_idx = op.btoi(op.extract(entries, UInt64(48), UInt64(8)))
            c4_sel = op.extract(entries, UInt64(56), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c2_idx),
                self.chunk_lens[c2_idx])
            self._stage_call(c2_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c3_idx),
                self.chunk_lens[c3_idx])
            self._stage_call(c3_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c4_idx),
                self.chunk_lens[c4_idx])
            self._stage_call(c4_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(9)
        elif chain_len == UInt64(6):
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            c2_idx = op.btoi(op.extract(entries, UInt64(24), UInt64(8)))
            c2_sel = op.extract(entries, UInt64(32), UInt64(4))
            c3_idx = op.btoi(op.extract(entries, UInt64(36), UInt64(8)))
            c3_sel = op.extract(entries, UInt64(44), UInt64(4))
            c4_idx = op.btoi(op.extract(entries, UInt64(48), UInt64(8)))
            c4_sel = op.extract(entries, UInt64(56), UInt64(4))
            c5_idx = op.btoi(op.extract(entries, UInt64(60), UInt64(8)))
            c5_sel = op.extract(entries, UInt64(68), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c2_idx),
                self.chunk_lens[c2_idx])
            self._stage_call(c2_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c3_idx),
                self.chunk_lens[c3_idx])
            self._stage_call(c3_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c4_idx),
                self.chunk_lens[c4_idx])
            self._stage_call(c4_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c5_idx),
                self.chunk_lens[c5_idx])
            self._stage_call(c5_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(11)
        else:
            assert chain_len == UInt64(7), "dispatch_chain: chain_len must be 2..7"
            c0_idx = op.btoi(op.extract(entries, UInt64(0), UInt64(8)))
            c0_sel = op.extract(entries, UInt64(8), UInt64(4))
            c1_idx = op.btoi(op.extract(entries, UInt64(12), UInt64(8)))
            c1_sel = op.extract(entries, UInt64(20), UInt64(4))
            c2_idx = op.btoi(op.extract(entries, UInt64(24), UInt64(8)))
            c2_sel = op.extract(entries, UInt64(32), UInt64(4))
            c3_idx = op.btoi(op.extract(entries, UInt64(36), UInt64(8)))
            c3_sel = op.extract(entries, UInt64(44), UInt64(4))
            c4_idx = op.btoi(op.extract(entries, UInt64(48), UInt64(8)))
            c4_sel = op.extract(entries, UInt64(56), UInt64(4))
            c5_idx = op.btoi(op.extract(entries, UInt64(60), UInt64(8)))
            c5_sel = op.extract(entries, UInt64(68), UInt64(4))
            c6_idx = op.btoi(op.extract(entries, UInt64(72), UInt64(8)))
            c6_sel = op.extract(entries, UInt64(80), UInt64(4))
            self._stage_install_first(
                Bytes(b"__codebox_chunk_") + op.itob(c0_idx),
                self.chunk_lens[c0_idx])
            self._stage_call(c0_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c1_idx),
                self.chunk_lens[c1_idx])
            self._stage_call(c1_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c2_idx),
                self.chunk_lens[c2_idx])
            self._stage_call(c2_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c3_idx),
                self.chunk_lens[c3_idx])
            self._stage_call(c3_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c4_idx),
                self.chunk_lens[c4_idx])
            self._stage_call(c4_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c5_idx),
                self.chunk_lens[c5_idx])
            self._stage_call(c5_sel)
            self._stage_install_next(
                Bytes(b"__codebox_chunk_") + op.itob(c6_idx),
                self.chunk_lens[c6_idx])
            self._stage_call(c6_sel)
            self._stage_install_next(default_box, default_len)
            itxn.submit_staged()
            return op.GITxn.last_log(13)

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
        #
        # apps=(main,): the chunk's body reads main's __og_sender /
        # __og_value globals via app_global_get_ex, and main's address
        # via app_params_get(MAIN, AppAddress). Both require main's app
        # id to be in the resource list of the inner txn that targets
        # __storage. CallerApplicationID at this frame is main (main's
        # forwarding stub is what dispatched into us), so we forward it
        # straight through.
        main_app = Application(op.Global.caller_application_id)
        n = op.Txn.num_app_args - UInt64(1)
        if n == UInt64(1):
            # Just selector, no extra args
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector,),
                apps=(main_app,),
                fee=0,
            ).submit()
        elif n == UInt64(2):
            a1 = op.Txn.application_args(UInt64(2))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1),
                apps=(main_app,),
                fee=0,
            ).submit()
        elif n == UInt64(3):
            a1 = op.Txn.application_args(UInt64(2))
            a2 = op.Txn.application_args(UInt64(3))
            call_res = itxn.ApplicationCall(
                app_id=target_app,
                on_completion=OnCompleteAction.NoOp,
                app_args=(user_selector, a1, a2),
                apps=(main_app,),
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
                apps=(main_app,),
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
                apps=(main_app,),
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
                apps=(main_app,),
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
