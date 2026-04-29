"""Box-read depth probe.

A single contract design that can act as either a "leaf" (reads its own
box) or a "relay" (inner-calls another DepthProbe and forwards the
result). Lets us build a chain user → A → B → C → ... → leaf and
measure whether `box_get` returns the expected value at each chain
length.

Used to isolate Group A's matchOrders bug: in the dance flow
(test → chunk → orch[+UpdateApp] → matchOrders → helper1 → CTFMock),
CTFMock's `box_get` at depth 4 returns 32 zero bytes despite the box
holding the funded value. We need to know whether *depth 4* alone is
the breaker, or whether the `UpdateApplication` hop is.
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


class DepthProbe(ARC4Contract):
    """Doubles as box-leaf (write/read its own balances) and relay
    (inner-call another DepthProbe and return its result)."""

    def __init__(self) -> None:
        # Box-backed map. Mirrors CTFMock's `b_<key>` shape so the bug
        # if any is at the box-availability layer, not the codegen.
        self.balances = BoxMap(Bytes, Bytes, key_prefix=b"b_")

    @arc4.abimethod(create="allow")
    def init(self) -> None:
        pass

    @arc4.abimethod(allow_actions=("UpdateApplication",))
    def __delegate_update(self) -> None:
        """No-op admit handler for the program-swap step. Mirrors the
        orch's `__delegate_update` in the lonely-chunk dance, so the
        UpdateApplication inner-call lands cleanly on the OLD program
        (still DepthProbe at that point)."""

    @arc4.abimethod
    def noop(self) -> None:
        """True no-op — touches no state, reads no boxes. Used as a pad
        txn target when we want to attach foreign-app box refs to a
        sibling txn without that txn doing any box access of its own."""

    @arc4.abimethod
    def setBalance(self, key: arc4.DynamicBytes, value: arc4.DynamicBytes) -> None:
        """Write `value` to `balances[key]`. Test harness pre-populates
        the leaf this way; matches the `mint` helper on USDC/CTFMock."""
        self.balances[key.native] = value.native

    @arc4.abimethod
    def getBalance(self, key: arc4.DynamicBytes) -> arc4.DynamicBytes:
        """Direct read — no inner-call. The probe bottom-of-chain
        method. If THIS reads zero when the box holds a value, the bug
        is structural to the AVM-vs-test-harness setup; if it reads
        the value correctly, depth-1 is fine."""
        raw = self.balances.get(key.native, default=Bytes(b""))
        return arc4.DynamicBytes(raw)

    @arc4.abimethod
    def relayGet(
        self,
        target_app: UInt64,
        key: arc4.DynamicBytes,
    ) -> arc4.DynamicBytes:
        """Inner-call `target_app.getBalance(key)` and forward the
        ABI-encoded result. Each `relayGet` adds one inner-tx hop, so
        a single `relayGet` puts the leaf's `box_get` at depth 2
        (user → relay → leaf)."""
        sel = arc4.arc4_signature("getBalance(byte[])byte[]")
        result = itxn.ApplicationCall(
            app_id=target_app,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel, key.bytes),
            fee=0,
        ).submit()
        # Strip ARC-28 4-byte log prefix (`0x151f7c75`) to recover the
        # ABI-encoded `byte[]` return value.
        return arc4.DynamicBytes.from_bytes(result.last_log[4:])

    @arc4.abimethod
    def relayChain2(
        self,
        intermediate_app: UInt64,
        final_app: UInt64,
        key: arc4.DynamicBytes,
    ) -> arc4.DynamicBytes:
        """user → this → intermediate.relayGet(final) → final.getBalance.
        Leaf's `box_get` runs at depth 3."""
        sel = arc4.arc4_signature("relayGet(uint64,byte[])byte[]")
        result = itxn.ApplicationCall(
            app_id=intermediate_app,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel, op.itob(final_app), key.bytes),
            fee=0,
        ).submit()
        return arc4.DynamicBytes.from_bytes(result.last_log[4:])

    @arc4.abimethod
    def relayChain3(
        self,
        mid1_app: UInt64,
        mid2_app: UInt64,
        final_app: UInt64,
        key: arc4.DynamicBytes,
    ) -> arc4.DynamicBytes:
        """user → this → mid1.relayChain2(mid2, final) → mid2.relayGet(final)
        → final.getBalance. Leaf's `box_get` runs at depth 4 — same as
        the matchOrders dance's CTFMock call."""
        sel = arc4.arc4_signature("relayChain2(uint64,uint64,byte[])byte[]")
        result = itxn.ApplicationCall(
            app_id=mid1_app,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel, op.itob(mid2_app), op.itob(final_app), key.bytes),
            fee=0,
        ).submit()
        return arc4.DynamicBytes.from_bytes(result.last_log[4:])

    @arc4.abimethod
    def relayUpdateThenChain3(
        self,
        intermediary: UInt64,
        new_p0: Bytes,
        helper: UInt64,
        leaf: UInt64,
        key: arc4.DynamicBytes,
    ) -> arc4.DynamicBytes:
        """Replicates the matchOrders dance shape exactly.

          user → this → intermediary[UpdateApp]   (depth 2)
               → this → intermediary.relayChain2(helper, leaf, key)
                            → helper.relayGet(leaf, key)
                                → leaf.getBalance  (depth 4)

        UpdateApplication is on the *intermediary*, two hops above the
        leaf whose box is read. If `test_depth_probe_chain3` passes
        (no UpdateApp, depth 4 reads cleanly) but THIS test fails, then
        UpdateApp on an intermediary is the matchOrders Group A breaker.
        """
        # 1. UpdateApp on intermediary — a no-op program swap (we pass
        #    DepthProbe's own program back in, so the contract identity
        #    is preserved).
        clear = Bytes(b"\x0c\x81\x01\x43")  # `pragma 12; pushint 1; return`
        delegate_sel = arc4.arc4_signature("__delegate_update()void")
        itxn.ApplicationCall(
            app_id=intermediary,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=new_p0,
            clear_state_program=clear,
            app_args=(delegate_sel,),
            fee=0,
        ).submit()

        # 2. Chain through the freshly-updated intermediary down to leaf.
        chain2_sel = arc4.arc4_signature("relayChain2(uint64,uint64,byte[])byte[]")
        result = itxn.ApplicationCall(
            app_id=intermediary,
            on_completion=OnCompleteAction.NoOp,
            app_args=(chain2_sel, op.itob(helper), op.itob(leaf), key.bytes),
            fee=0,
        ).submit()
        return arc4.DynamicBytes.from_bytes(result.last_log[4:])

    @arc4.abimethod
    def relayUpdateAndCall(
        self,
        target_app: UInt64,
        new_program_p0: Bytes,
        new_program_p1: Bytes,
        key: arc4.DynamicBytes,
    ) -> arc4.DynamicBytes:
        """Like `relayGet`, but first does `UpdateApplication` on
        `target_app` with `new_program_p0 ++ new_program_p1` as the new
        approval program (and the same trivial clear program the
        lonely-chunk dance uses). Then calls `getBalance(key)` on the
        now-replaced target_app's program.

        The point is to measure whether the `UpdateApplication` hop is
        load-bearing for the depth-4 box-read failure: if the bug
        reproduces here at depth 2 (user → DepthProbe → target), the
        `UpdateApplication` swap itself is the breaker. If it doesn't
        reproduce, the matchOrders dance is failing for some other
        reason (combined with depth)."""
        # 1. UpdateApplication on target_app, replacing approval program.
        clear = Bytes(b"\x0c\x81\x01\x43")  # `pragma 12; pushint 1; return`
        # ARC-4 selector for the orch's `__delegate_update()void` —
        # we re-use the same hook the lonely chunk hits so the program
        # swap lands cleanly.
        delegate_sel = Bytes(b"\xdc\x5e\x37\x98")
        itxn.ApplicationCall(
            app_id=target_app,
            on_completion=OnCompleteAction.UpdateApplication,
            approval_program=(new_program_p0, new_program_p1),
            clear_state_program=clear,
            app_args=(delegate_sel,),
            fee=0,
        ).submit()

        # 2. Now call the freshly-updated target_app with `getBalance`.
        sel = arc4.arc4_signature("getBalance(byte[])byte[]")
        result = itxn.ApplicationCall(
            app_id=target_app,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel, key.bytes),
            fee=0,
        ).submit()
        return arc4.DynamicBytes.from_bytes(result.last_log[4:])
