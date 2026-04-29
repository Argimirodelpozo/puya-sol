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
