"""Group A diagnostic: at what call depth does `box_get` start
returning the wrong (zero) value, and does the `UpdateApplication`
hop matter?

Setup: deploy N+1 instances of `DepthProbe`. The first acts as the
"leaf" (holds a populated `b_<key>` box). Each subsequent instance
relays into the previous one via inner-tx, so chaining
`relayGet` N times and ending at `getBalance` puts the leaf's
`box_get` at depth N+1 from the user's outer txn.

Comparable matchOrders dance:
  test → chunk → orch[+UpdateApp] → matchOrders → helper1 → CTFMock
  depth: 1     →   2 (twice)      →    2          →   3      →   4
The leaf `box_get` happens at depth 4. We replicate that depth
straight (no `UpdateApplication`) AND with an `UpdateApplication`
mid-chain to isolate the variable.
"""
from pathlib import Path

import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, app_id_to_address
from dev.arc56 import compile_teal, load_arc56
from dev.deploy import AUTO_POPULATE, create_app


PROBE_OUT = Path(__file__).parent.parent.parent / "delegate" / "out"


@pytest.fixture(scope="function")
def probe_factory(localnet, admin):
    """Returns a callable that deploys a fresh `DepthProbe` instance
    and returns its `AppClient`. Each call creates a new app id, so
    chains can be wired without aliasing."""
    spec = load_arc56(PROBE_OUT / "DepthProbe.arc56.json")
    algod = localnet.client.algod
    approval = compile_teal(algod, (PROBE_OUT / "DepthProbe.approval.teal").read_text())
    clear = compile_teal(algod, (PROBE_OUT / "DepthProbe.clear.teal").read_text())

    def make():
        # `init()` is `create="allow"` — the router still requires the
        # init selector on the create txn, so pass it as ApplicationArgs[0].
        init_sel = b"\x83\xf1\x47\x48"  # sha512_256("init()void")[:4]
        app_id = create_app(
            localnet, admin, approval, clear,
            spec.state.schema.global_state,
            app_args=[init_sel],
        )
        return au.AppClient(au.AppClientParams(
            algorand=localnet, app_spec=spec, app_id=app_id,
            default_sender=admin.address,
        ))
    return make


KEY = b"probe-key"
VALUE = b"\x00" * 24 + (12345).to_bytes(8, "big")  # arbitrary 32-byte value


def _set_balance(probe, k=KEY, v=VALUE):
    probe.send.call(au.AppClientMethodCallParams(
        method="setBalance", args=[k, v],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + k)],
    ), send_params=AUTO_POPULATE)


def _read_chain(relays, leaf, key=KEY):
    """Call the FIRST relay's `relayGet` with `target_app` = next link
    in the chain. Each relay forwards via `relayGet`, except the very
    last hop which lands on `leaf.getBalance`. Return whatever bytes
    came back from the leaf's box read."""
    if not relays:
        return leaf.send.call(au.AppClientMethodCallParams(
            method="getBalance", args=[key],
            extra_fee=au.AlgoAmount(micro_algo=20_000),
            box_references=[au.BoxReference(app_id=0, name=b"b_" + key)],
        ), send_params=AUTO_POPULATE).abi_return
    # Chain: relays[0] → relays[1] → ... → leaf
    # We call relays[0].relayGet(relays[1].app_id, key); but relayGet
    # only does one inner-call. To chain N relays, each relayGet's
    # target IS the next relay (whose `getBalance` would short-circuit
    # to its OWN box). So this design currently only tests depth 2 in
    # one shot.
    raise NotImplementedError(
        "chained relayGet not yet wired — see _read_via_one_relay below"
    )


def _read_via_one_relay(relay, leaf, key=KEY, extra_fee=80_000):
    """Depth-2 read: user → relay.relayGet → leaf.getBalance.
    The leaf's `box_get` runs at depth 2."""
    res = relay.send.call(au.AppClientMethodCallParams(
        method="relayGet", args=[leaf.app_id, key],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        app_references=[leaf.app_id],
        box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + key)],
    ), send_params=AUTO_POPULATE).abi_return
    return bytes(res) if isinstance(res, (list, tuple, bytes, bytearray)) else res


def test_depth_probe_direct(probe_factory):
    """Sanity: depth-1 (`user → leaf.getBalance`) should read back the
    written value. Confirms the harness + box ref wiring."""
    leaf = probe_factory()
    _set_balance(leaf)
    out = leaf.send.call(au.AppClientMethodCallParams(
        method="getBalance", args=[KEY],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + KEY)],
    ), send_params=AUTO_POPULATE).abi_return
    out_b = bytes(out) if isinstance(out, (list, tuple, bytes, bytearray)) else out
    assert out_b == VALUE, f"depth-1 read returned {out_b!r}, expected {VALUE!r}"


def test_depth_probe_one_relay(probe_factory):
    """Depth 2: `user → relay → leaf` — leaf's `box_get` runs at
    depth 2. Should match the funded value."""
    leaf = probe_factory()
    relay = probe_factory()
    _set_balance(leaf)
    out = _read_via_one_relay(relay, leaf)
    assert out == VALUE, f"depth-2 read returned {out!r}, expected {VALUE!r}"


def test_depth_probe_chain2(probe_factory):
    """Depth 3: `user → R0 → R1 → leaf` — leaf's `box_get` runs at
    depth 3. Same depth as the matchOrders dance's helper1 inner-call
    (user → chunk → orch → helper1) BEFORE the helper1 → CTFMock hop."""
    leaf = probe_factory()
    r1 = probe_factory()
    r0 = probe_factory()
    _set_balance(leaf)
    res = r0.send.call(au.AppClientMethodCallParams(
        method="relayChain2", args=[r1.app_id, leaf.app_id, KEY],
        extra_fee=au.AlgoAmount(micro_algo=120_000),
        app_references=[r1.app_id, leaf.app_id],
        box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
    ), send_params=AUTO_POPULATE).abi_return
    out = bytes(res) if isinstance(res, (list, tuple, bytes, bytearray)) else res
    assert out == VALUE, f"depth-3 read returned {out!r}, expected {VALUE!r}"


def test_depth_probe_chain3(probe_factory):
    """Depth 4: `user → R0 → R1 → R2 → leaf` — leaf's `box_get` runs
    at depth 4. THIS is the depth at which the matchOrders dance's
    CTFMock fails (chunk → orch → matchOrders body → helper1 → CTFMock).
    If THIS test passes, depth alone isn't the breaker — the dance's
    `UpdateApplication` step is the culprit."""
    leaf = probe_factory()
    r2 = probe_factory()
    r1 = probe_factory()
    r0 = probe_factory()
    _set_balance(leaf)
    res = r0.send.call(au.AppClientMethodCallParams(
        method="relayChain3",
        args=[r1.app_id, r2.app_id, leaf.app_id, KEY],
        extra_fee=au.AlgoAmount(micro_algo=160_000),
        app_references=[r1.app_id, r2.app_id, leaf.app_id],
        box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
    ), send_params=AUTO_POPULATE).abi_return
    out = bytes(res) if isinstance(res, (list, tuple, bytes, bytearray)) else res
    assert out == VALUE, f"depth-4 read returned {out!r}, expected {VALUE!r}"


def _depth_probe_program() -> bytes:
    """The compiled approval program of `DepthProbe`. We pass it back
    into `UpdateApplication` so the swap is identity-preserving — the
    target_app remains a DepthProbe after the swap."""
    return (PROBE_OUT / "DepthProbe.approval.bin").read_bytes()


def test_depth_probe_update_then_call(probe_factory):
    """Depth 2 with `UpdateApplication` on the leaf itself.

      user → relay → leaf[UpdateApp] (depth 2)
           → relay → leaf.getBalance (depth 2, reads leaf's box)

    Uses `relayUpdateAndCall`, which inner-calls `target.UpdateApp` with
    DepthProbe's own bytes (no-op identity swap), then `target.getBalance`.
    If this returns 32 zero bytes, an UpdateApplication on the call
    target invalidates that target's subsequent box reads — that would
    explain matchOrders only if CTFMock itself were UpdateApp'd, which
    it isn't, so this is mostly a sanity baseline."""
    leaf = probe_factory()
    relay = probe_factory()
    _set_balance(leaf)
    program = _depth_probe_program()
    # `relayUpdateAndCall` splits the new program over two ABI args; the
    # AVM concatenates them into a single approval program, so passing
    # `(program, b"")` is equivalent to a single full chunk.
    res = relay.send.call(au.AppClientMethodCallParams(
        method="relayUpdateAndCall",
        args=[leaf.app_id, program, b"", KEY],
        extra_fee=au.AlgoAmount(micro_algo=120_000),
        app_references=[leaf.app_id],
        box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
    ), send_params=AUTO_POPULATE).abi_return
    out = bytes(res) if isinstance(res, (list, tuple, bytes, bytearray)) else res
    assert out == VALUE, (
        f"depth-2-with-UpdateApp read returned {out!r}, expected {VALUE!r} "
        f"— UpdateApplication on the call target broke its own box read"
    )


def test_depth_probe_chain3_box_ref_on_sibling_txn(probe_factory, admin):
    """Depth-4 box read where the box reference is on a SIBLING txn,
    not the dance txn itself.

    `dance_match_orders` distributes inner-call box refs across pad txns
    (target = orch.isAdmin), keeping only chunk's own boxes on the
    actual dance txn. Per AVM rules box availability is pooled across
    the group, so this should still work — but if it doesn't, that's
    the matchOrders Group A breaker."""
    leaf = probe_factory()
    r2 = probe_factory()
    r1 = probe_factory()
    r0 = probe_factory()
    _set_balance(leaf)

    composer = r0.algorand.new_group()
    composer.add_app_call_method_call(r0.params.call(
        au.AppClientMethodCallParams(
            method="noop",  # touches no state, reads no boxes
            args=[],
            note=b"sibling-with-box-ref",
            box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
            app_references=[leaf.app_id],
        )))
    composer.add_app_call_method_call(r0.params.call(
        au.AppClientMethodCallParams(
            method="relayChain3",
            args=[r1.app_id, r2.app_id, leaf.app_id, KEY],
            extra_fee=au.AlgoAmount(micro_algo=160_000),
            app_references=[r1.app_id, r2.app_id, leaf.app_id],
        )))
    res = composer.send(au.SendParams(populate_app_call_resources=False))
    raw = res.returns[-1].value
    out = bytes(raw) if isinstance(raw, (list, tuple, bytes, bytearray)) else raw
    assert out == VALUE, (
        f"depth-4 read with box ref on SIBLING txn returned {out!r}, "
        f"expected {VALUE!r} — AVM didn't pool the box ref across the group"
    )


def test_depth_probe_full_dance_shape(probe_factory, admin):
    """Complete dance-shape replication: outer group has pad txns
    (target: r0.noop) carrying the leaf box ref + the dance txn calling
    `relayUpdateThenChain3` (UpdateApp on intermediary, then depth-4
    chain to leaf).box_references are deliberately split across sibling
    txns the way `dance_match_orders` does it.

    If this PASSES while `test_match_orders_complementary` FAILS, the
    matchOrders bug is NOT about depth, NOT about UpdateApplication,
    NOT about cross-txn box pooling — it's about something specific
    to the CTFExchange/Helper/CTFMock chain (encoding, address shape,
    or program-swap interaction with split helpers)."""
    leaf = probe_factory()
    helper = probe_factory()
    intermediary = probe_factory()
    r0 = probe_factory()
    _set_balance(leaf)
    program = _depth_probe_program()

    composer = r0.algorand.new_group()
    composer.add_app_call_method_call(r0.params.call(
        au.AppClientMethodCallParams(
            method="noop",
            args=[],
            note=b"pad-with-leaf-box-ref",
            box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
            app_references=[leaf.app_id],
        )))
    composer.add_app_call_method_call(r0.params.call(
        au.AppClientMethodCallParams(
            method="relayUpdateThenChain3",
            args=[intermediary.app_id, program, helper.app_id, leaf.app_id, KEY],
            extra_fee=au.AlgoAmount(micro_algo=200_000),
            app_references=[intermediary.app_id, helper.app_id, leaf.app_id],
        )))
    res = composer.send(au.SendParams(populate_app_call_resources=False))
    raw = res.returns[-1].value
    out = bytes(raw) if isinstance(raw, (list, tuple, bytes, bytearray)) else raw
    assert out == VALUE, (
        f"full-dance-shape read returned {out!r}, expected {VALUE!r} "
        f"— the matchOrders mystery is upstream of these primitives"
    )


def test_depth_probe_update_then_chain3(probe_factory):
    """Faithful replication of the matchOrders dance shape.

      user → R0 → intermediary[UpdateApp]               (depth 2)
           → R0 → intermediary.relayChain2(helper, leaf, key)
                       → helper.relayGet(leaf, key)
                              → leaf.getBalance         (depth 4)

    Mirrors `chunk → orch[UpdateApp] → orch.matchOrders → helper1 →
    CTFMock`. UpdateApplication is on the intermediary (orch-equivalent)
    two hops above the leaf whose box is read. If `test_depth_probe_chain3`
    passes (no UpdateApp at depth 4) but THIS fails, the dance's
    `UpdateApplication` step is the matchOrders Group A breaker."""
    leaf = probe_factory()
    helper = probe_factory()
    intermediary = probe_factory()
    r0 = probe_factory()
    _set_balance(leaf)
    program = _depth_probe_program()
    res = r0.send.call(au.AppClientMethodCallParams(
        method="relayUpdateThenChain3",
        args=[intermediary.app_id, program, helper.app_id, leaf.app_id, KEY],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
        app_references=[intermediary.app_id, helper.app_id, leaf.app_id],
        box_references=[au.BoxReference(app_id=leaf.app_id, name=b"b_" + KEY)],
    ), send_params=AUTO_POPULATE).abi_return
    out = bytes(res) if isinstance(res, (list, tuple, bytes, bytearray)) else res
    assert out == VALUE, (
        f"depth-4-with-UpdateApp-on-intermediary read returned {out!r}, "
        f"expected {VALUE!r} — this is the smoking gun for matchOrders"
    )
