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


def test_match_orders_diagnostic_dump_args(split_settled_with_delegate):
    """Run the dance via simulate (which fails the same way as send), then
    walk the simulate_response['txn-groups'][0]['txn-results'] to find
    the failing inner-tx and dump its ApplicationArgs. Identifies what
    `from` helper3 is actually passing to helper1 (and helper1 to ctf)."""
    from hashlib import sha256
    import base64
    from algosdk.v2client.models.simulate_request import (
        SimulateRequest, SimulateRequestTransactionGroup, SimulateTraceConfig,
    )
    from algosdk.atomic_transaction_composer import (
        EmptySigner, AtomicTransactionComposer,
    )
    from dev.deals import (deal_outcome, deal_outcome_and_approve,
        deal_usdc_and_approve, prepare_condition, set_approval, ctf_balance)
    from dev.match_dispatch import (encode_match_orders_args, DELEGATE_UPDATE_SEL,
        MATCH_ORDERS_SEL)
    from dev.orders import make_order, sign_order, Side
    from dev.signing import bob as bob_signer_fn, carla as carla_signer_fn
    from dev.addrs import algod_addr_bytes_for_app

    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob_signer_fn()
    carla_signer = carla_signer_fn()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    # Compute canonical YES_ID/NO_ID via helper1 (matchOrders validates
    # tokenIds against `getPositionId(ctfCollateral, getCollectionId(0,
    # conditionId, indexSet))`).
    raw_coll = orch.send.call(au.AppClientMethodCallParams(
        method="getCtfCollateral", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(raw_coll, str):
        from algosdk.encoding import decode_address
        ctf_coll = decode_address(raw_coll)
    else:
        ctf_coll = bytes(raw_coll)

    def _pid(idx_set):
        cid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getCollectionId",
            args=[list(b"\x00" * 32), list(b"\xc0" * 32), idx_set],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        cid_b = bytes(cid) if isinstance(cid, (list, tuple)) else bytes(cid)
        pid = h1.send.call(au.AppClientMethodCallParams(
            method="CTHelpers.getPositionId",
            args=[bytes(ctf_coll), list(cid_b)],
            extra_fee=au.AlgoAmount(micro_algo=500_000),
        ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
        return int(pid)
    YES_ID = _pid(1)
    NO_ID = _pid(2)
    prepare_condition(ctf, b"\xc0" * 32, YES_ID, NO_ID)
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 50_000_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, YES_ID, 100_000_000)

    taker = make_order(maker=bob_addr, token_id=YES_ID,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    maker = make_order(maker=carla_addr, token_id=YES_ID,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    a1, a2, a3, a4, a5, a6, a7 = encode_match_orders_args(
        condition_id=b"\xc0" * 32,
        taker_order_list=taker_signed.to_abi_list(),
        maker_orders_list=[maker_signed.to_abi_list()],
        taker_fill_amount=50_000_000,
        maker_fill_amounts=[100_000_000],
        taker_fee_amount=0,
        maker_fee_amounts=[0],
    )

    # Build the dance group.
    composer = chunk.algorand.new_group()
    yes_bytes = YES_ID.to_bytes(32, "big")
    inner_boxes = [
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(bob_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(carla_addr)),
    ]
    # Match dance_match_orders: 6 boxes on pad-0, rest empty.
    PER_PAD = 6
    for i in range(15):
        slot_boxes = inner_boxes[i*PER_PAD:(i+1)*PER_PAD] if i*PER_PAD < len(inner_boxes) else []
        slot_app_ids = sorted({b.app_index for b in slot_boxes if b.app_index != 0})
        composer.add_app_call_method_call(orch.params.call(
            au.AppClientMethodCallParams(
                method="isAdmin",
                args=[b"\x00" * 32],
                note=f"opup-{i}".encode(),
                box_references=slot_boxes if slot_boxes else None,
                app_references=slot_app_ids if slot_app_ids else None,
            )))
    composer.add_app_call_method_call(chunk.params.call(
        au.AppClientMethodCallParams(
            method="dance_call_7",
            args=[DELEGATE_UPDATE_SEL, MATCH_ORDERS_SEL,
                  a1, a2, a3, a4, a5, a6, a7],
            extra_fee=au.AlgoAmount(micro_algo=5_000_000),
            app_references=[orch.app_id, usdc.app_id, ctf.app_id, h1.app_id],
            box_references=[
                au.BoxReference(app_id=0, name=b"__self_bytes"),
                au.BoxReference(app_id=0, name=b"__orch_orig_bytes"),
            ],
        )))

    # Diagnostic: read carla's CTF balance box via algod BEFORE the
    # dance fires. If the box content here != 100M, the deal didn't
    # actually populate it for this YES_ID. If 100M, then matchOrders
    # truly is reading bal=0 from a non-empty box.
    yes_id_bytes = YES_ID.to_bytes(32, "big")
    expected_key = b"b_" + sha256(bytes(carla_addr) + yes_id_bytes).digest()
    print(f"\nDeal expected key: {expected_key.hex()}")
    print(f"YES_ID:            {YES_ID:#x}")
    print(f"YES_ID hex:        {yes_id_bytes.hex()}")
    print(f"carla_addr:        {bytes(carla_addr).hex()}")
    box_raw = chunk.algorand.client.algod.application_box_by_name(
        ctf.app_id, expected_key)
    box_value_b64 = box_raw.get("value", "")
    box_value = base64.b64decode(box_value_b64) if box_value_b64 else b""
    print(f"Box content (algod): {box_value.hex()}")
    print(f"Box content length: {len(box_value)}")
    if len(box_value) >= 32:
        as_int = int.from_bytes(box_value[24:32], "big")
        print(f"Decoded balance (last 8 bytes BE): {as_int}")

    composer.build()  # populates _atc
    from algokit_utils._debugging import simulate_response as _sim_resp
    algod = chunk.algorand.client.algod
    sim_resp = _sim_resp(
        composer._atc, algod,
        allow_more_logs=True, allow_unnamed_resources=True,
        exec_trace_config=SimulateTraceConfig(enable=True, stack_change=True, scratch_change=False),
    )
    raw = sim_resp.simulate_response
    txn_groups = raw["txn-groups"]
    print(f"\n=== SIMULATE FAILURE ===")
    if txn_groups[0].get("failure-message"):
        print(f"Failure: {txn_groups[0]['failure-message']}")
        print(f"Failed-at: {txn_groups[0].get('failed-at')}")

    # The dance fails at [15, 1, ..., 0]. Walk to the FAILING inner-tx.
    def get_apaa(node):
        # Try both shapes: outer wrapped in "txn-result", inner direct.
        for path in [["txn", "txn", "apaa"], ["txn-result", "txn", "txn", "apaa"]]:
            cur = node
            for p in path:
                if isinstance(cur, dict):
                    cur = cur.get(p)
                else:
                    cur = None
                    break
            if cur:
                return cur
        return None

    def walk_to_failure(node, path):
        if not path:
            return node
        idx = path[0]
        rest = path[1:]
        inners = node.get("inner-txns") or []
        if idx >= len(inners):
            return None
        return walk_to_failure(inners[idx], rest)

    # Dump box references on every txn in the group.
    print("\n=== Box references per txn ===")
    for i, tr in enumerate(txn_groups[0].get("txn-results", [])):
        txn = tr.get("txn-result", {}).get("txn", {}).get("txn", {})
        apbx = txn.get("apbx") or []
        if apbx:
            box_refs = []
            for ref in apbx:
                idx = ref.get("i", 0)  # 0 = current app, 1+ = foreign apps
                name_b64 = ref.get("n", "")
                name_b = base64.b64decode(name_b64) if name_b64 else b""
                box_refs.append(f"i={idx} name={name_b.hex()[:80]}")
            print(f"  txn {i}: {len(apbx)} box refs: {box_refs}")
        else:
            print(f"  txn {i}: 0 box refs")

    failed_at = txn_groups[0].get("failed-at", [])
    print(f"\nFailed-at path: {failed_at}")
    if failed_at and len(failed_at) >= 1:
        outer_idx = failed_at[0]
        outer = txn_groups[0]["txn-results"][outer_idx]
        print(f"\n[T{outer_idx} top-level keys]: {list(outer.keys())}")
        # Walk along the failed-at path, printing apaa at each level.
        def dive(node, path_so_far, remaining):
            keys_seen = list(node.keys()) if isinstance(node, dict) else []
            ap = get_apaa(node)
            apaa_str = [base64.b64decode(a).hex() if a else '' for a in (ap or [])]
            print(f"  [{path_so_far}] keys={keys_seen[:8]}... apaa={apaa_str if apaa_str else 'NONE'}")
            if not remaining:
                return
            # Inner-txns may be nested under "txn-result" or directly.
            inners = node.get("inner-txns") or (
                node.get("txn-result", {}).get("inner-txns")
            ) or []
            print(f"    inner-txns: {len(inners)} entries")
            if remaining[0] < len(inners):
                dive(inners[remaining[0]],
                     path_so_far + f".in{remaining[0]}",
                     remaining[1:])
        dive(outer, f"T{outer_idx}", failed_at[1:])

    # Walk to the deepest inner-tx exec trace and dump opcodes near box_get.
    def get_trace(node, path):
        if not path:
            return node
        idx = path[0]
        rest = path[1:]
        et = node.get("exec-trace") or node.get("txn-result", {}).get("exec-trace")
        if not et:
            return None
        inners = et.get("inner-trace") or []
        if idx < len(inners):
            return get_trace(inners[idx], rest)
        return None

    def get_outer_trace(outer):
        return outer.get("exec-trace")

    def find_box_get_in_trace(trace, label):
        """Walk approval-program-trace and dump LAST 30 ops to see what
        was on stack before the failing assert."""
        ops = trace.get("approval-program-trace") or []
        print(f"  {label}: {len(ops)} ops in trace")
        # Show last 30 ops (around the failure).
        for i, op in enumerate(ops[-30:]):
            pc = op.get("pc", 0)
            stack_add = op.get("stack-additions") or []
            stack_pop = op.get("stack-pop-count", 0)
            added = []
            for s in stack_add:
                if "bytes" in s:
                    added.append(("b", base64.b64decode(s["bytes"]).hex()))
                elif "uint" in s:
                    added.append(("u", s["uint"]))
            print(f"    pc={pc} pop={stack_pop} push={added}")

    if failed_at and len(failed_at) >= 1:
        outer = txn_groups[0]["txn-results"][failed_at[0]]
        outer_trace = get_outer_trace(outer)
        if outer_trace and len(failed_at) > 1:
            # Drill into nested inner traces.
            cur_trace = outer_trace
            for i, idx in enumerate(failed_at[1:]):
                inners = cur_trace.get("inner-trace") or []
                if idx < len(inners):
                    cur_trace = inners[idx]
                else:
                    print(f"!! No inner-trace[{idx}] at depth {i}")
                    cur_trace = None
                    break
            if cur_trace:
                print(f"\n=== Approval trace of failing inner-tx ({failed_at}) ===")
                find_box_get_in_trace(cur_trace, "FAIL_TX")

        # Also dump inner-tx 155 (the one right before 156=failure) to see
        # what matchOrders did just before. It might mutate the box.
        if outer_trace and len(failed_at) >= 3 and failed_at[2] >= 1:
            cur = outer_trace
            for idx in failed_at[1:1] + [failed_at[1]]:  # just go to in1
                inners = cur.get("inner-trace") or []
                cur = inners[idx] if idx < len(inners) else None
                if not cur:
                    break
            # Now dump prior inner txns at this level to find writes to ctf box.
            if cur:
                inners = cur.get("inner-trace") or []
                target_idx = failed_at[2]  # 156
                txn_results = txn_groups[0]["txn-results"][failed_at[0]].get("txn-result", {}).get("inner-txns", [])
                if target_idx > 0 and target_idx <= len(txn_results[failed_at[1]].get("inner-txns", [])):
                    matchords_inners = txn_results[failed_at[1]].get("inner-txns", [])
                    # Find ALL inner-txns going to ctf (the failing app).
                    fail_app = txn_groups[0].get("txn-results")[failed_at[0]].get("txn-result", {}).get("txn", {}).get("txn", {})
                    # Find ctf app id from the inner-tx 156's app
                    target_inner = matchords_inners[target_idx]
                    target_inner_inner = target_inner.get("inner-txns") or []
                    if target_inner_inner:
                        ctf_id = target_inner_inner[0].get("txn", {}).get("txn", {}).get("apid", 0)
                        print(f"\nctf app_id = {ctf_id}")
                        # Walk all matchOrders inner-txns looking for ones to ctf.
                        for n, mi in enumerate(matchords_inners):
                            mi_apid = mi.get("txn", {}).get("txn", {}).get("apid", 0)
                            mi_apaa = mi.get("txn", {}).get("txn", {}).get("apaa") or []
                            if mi_apid == ctf_id:
                                first_two = [base64.b64decode(a).hex()[:24] for a in mi_apaa[:2]]
                                print(f"  Inner-tx {n} -> ctf({mi_apid}): {first_two}")
                            # Also check if any inner-tx has further nested inner-tx to ctf
                            for j, nested in enumerate(mi.get("inner-txns") or []):
                                n_apid = nested.get("txn", {}).get("txn", {}).get("apid", 0)
                                n_apaa = nested.get("txn", {}).get("txn", {}).get("apaa") or []
                                if n_apid == ctf_id:
                                    full_args = [base64.b64decode(a).hex() for a in n_apaa]
                                    print(f"  Inner-tx {n}.{j} -> ctf({n_apid}):")
                                    for ai, av in enumerate(full_args):
                                        print(f"    arg[{ai}]: {av}")

    pytest.skip("Diagnostic only — see captured stdout for inner-tx args")


@pytest.mark.xfail(
    reason="Diagnostic for Group A: matchOrders fires CTF transfer twice "
           "with same args. Pre-funding bob/zero/etc doesn't help because "
           "carla's box gets debited to 0 by the first call, then the second "
           "fails. Fix in puya-sol's _settleComplementaryMaker codegen.",
    strict=False,
)
def test_match_orders_diagnostic_double_fund(split_settled_with_delegate):
    """Diagnostic: pre-fund BOTH carla AND bob with YES tokens, then
    run the matchOrders dance. The bal>=amt assert STILL fires — proving
    the `from` is neither carla, bob, nor zero. Combined with
    `test_match_orders_diagnostic_dump_args` (which inspects the inner-tx
    apaa via simulate exec_trace), this nailed down the actual root cause:
    matchOrders fires the CTF transfer TWICE with identical args."""
    from hashlib import sha256
    from dev.deals import deal_outcome, deal_outcome_and_approve, deal_usdc_and_approve, prepare_condition, set_approval, ctf_balance, usdc_balance
    from dev.match_dispatch import dance_match_orders
    from dev.orders import make_order, sign_order, Side
    from dev.signing import bob as bob_signer_fn, carla as carla_signer_fn
    from dev.addrs import algod_addr_bytes_for_app

    h1, _, orch, usdc, ctf, chunk = split_settled_with_delegate
    bob_signer = bob_signer_fn()
    carla_signer = carla_signer_fn()
    bob_addr = bob_signer.eth_address_padded32
    carla_addr = carla_signer.eth_address_padded32

    # Compute canonical YES/NO ids the same way test_match_orders does.
    raw = orch.send.call(au.AppClientMethodCallParams(
        method="getCtfCollateral", args=[],
        extra_fee=au.AlgoAmount(micro_algo=200_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    if isinstance(raw, str):
        from algosdk.encoding import decode_address
        ctf_collateral = decode_address(raw)
    else:
        ctf_collateral = bytes(raw)

    coll_id = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getCollectionId",
        args=[list(b"\x00" * 32), list(b"\xc0" * 32), 1],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    coll_b = bytes(coll_id) if isinstance(coll_id, (list, tuple)) else (
        bytes(coll_id) if not isinstance(coll_id, bytes) else coll_id)
    yes_id = int(h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getPositionId",
        args=[bytes(ctf_collateral), list(coll_b)],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return)
    coll_no = h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getCollectionId",
        args=[list(b"\x00" * 32), list(b"\xc0" * 32), 2],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return
    coll_no_b = bytes(coll_no) if isinstance(coll_no, (list, tuple)) else bytes(coll_no)
    no_id = int(h1.send.call(au.AppClientMethodCallParams(
        method="CTHelpers.getPositionId",
        args=[bytes(ctf_collateral), list(coll_no_b)],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
    ), send_params=au.SendParams(populate_app_call_resources=True)).abi_return)

    prepare_condition(ctf, b"\xc0" * 32, yes_id, no_id)
    h1_addr = algod_addr_bytes_for_app(h1.app_id)
    deal_usdc_and_approve(usdc, bob_addr, h1_addr, 50_000_000)
    deal_outcome_and_approve(ctf, carla_addr, h1_addr, yes_id, 100_000_000)
    # Diagnostic: pre-fund every plausible "wrong from" address.
    # If matchOrders reads bal=0 despite all of them funded, the
    # actual `from` is something we can't predict statically.
    deal_outcome(ctf, bob_addr, yes_id, 100_000_000)
    deal_outcome(ctf, b"\x00" * 32, yes_id, 100_000_000)
    # If `extract 32 32` on makerOrder accidentally reads offset 0, the
    # bytes would be `salt` (= 1, encoded as 31 zeros + 0x01).
    deal_outcome(ctf, b"\x00" * 31 + b"\x01", yes_id, 100_000_000)
    # signer == carla for EOA orders, but check anyway.
    # signer of taker == bob.
    # If the offset is 64 (signer field), that's `00...00 + bob_eth`.
    # That equals bob_addr already (already funded above).
    # If `extract 32 32` accidentally reads offset 96 (tokenId), bytes
    # would be yes_id-as-32-bytes-BE.
    yes_id_as_addr = yes_id.to_bytes(32, "big")
    deal_outcome(ctf, yes_id_as_addr, yes_id, 100_000_000)
    # If offset 128 (makerAmount), that's 100_000_000 as 32 BE bytes.
    maker_amount_as_addr = (100_000_000).to_bytes(32, "big")
    deal_outcome(ctf, maker_amount_as_addr, yes_id, 100_000_000)
    # If offset 160 (takerAmount), that's 50_000_000 as 32 BE bytes.
    taker_amount_as_addr = (50_000_000).to_bytes(32, "big")
    deal_outcome(ctf, taker_amount_as_addr, yes_id, 100_000_000)

    taker = make_order(maker=bob_addr, token_id=yes_id,
        maker_amount=50_000_000, taker_amount=100_000_000, side=Side.BUY)
    maker = make_order(maker=carla_addr, token_id=yes_id,
        maker_amount=100_000_000, taker_amount=50_000_000, side=Side.SELL)
    taker_signed = sign_order(orch, taker, bob_signer)
    maker_signed = sign_order(orch, maker, carla_signer)

    yes_bytes = yes_id.to_bytes(32, "big")
    zero_addr = b"\x00" * 32
    inner_boxes = [
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(zero_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256((b"\x00" * 31 + b"\x01") + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(yes_id_as_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(maker_amount_as_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"b_" + sha256(taker_amount_as_addr + yes_bytes).digest()),
        au.BoxReference(app_id=ctf.app_id,
                        name=b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id,
                        name=b"a_" + sha256(bytes(bob_addr) + h1_addr).digest()),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(bob_addr)),
        au.BoxReference(app_id=usdc.app_id, name=b"b_" + bytes(carla_addr)),
    ]
    try:
        dance_match_orders(
            chunk, orch,
            condition_id=b"\xc0" * 32,
            taker_order_list=taker_signed.to_abi_list(),
            maker_orders_list=[maker_signed.to_abi_list()],
            taker_fill_amount=50_000_000,
            maker_fill_amounts=[100_000_000],
            taker_fee_amount=0,
            maker_fee_amounts=[0],
            extra_app_refs=[usdc.app_id, ctf.app_id, h1.app_id],
            extra_box_refs=inner_boxes,
        )
    except Exception as e:
        msg = str(e)
        if "insufficient ERC1155 balance" in msg:
            # The bal>=amt check fired. Means matchOrders read a box with
            # bal=0 even though BOTH carla and bob have 100M. Either the
            # `from` address derivation is producing a third (unfunded)
            # address, or the read truly returns zero from a populated box.
            pytest.fail(
                "matchOrders still hit bal>=amt assert despite both "
                "carla AND bob being pre-funded with 100M YES. The `from` "
                "address must resolve to neither carla nor bob — likely "
                "an encoding or zero-padding mismatch."
            )
        raise


def test_depth_probe_h1_transfer_at_depth_3(probe_factory, split_settled_with_delegate):
    """Calls `helper1.TransferHelper._transferFromERC1155` *through one
    relay hop*, putting CTFMock at depth 3 — same depth at which
    matchOrders' settlement reads CTFMock and fails.

    Setup matches `test_match_orders_complementary` (deal 100M YES to
    carla, approve helper1 as operator). The ONLY difference vs the
    matchOrders dance's failing CTF transfer is who's constructing the
    `from`/`to`/`id`/`amount` ApplicationArgs going into helper1: the
    DepthProbe stub (this test) vs helper3's `_settleComplementaryMaker`
    (matchOrders).

    If this test PASSES while `test_match_orders_complementary` FAILS,
    helper3's matchOrders body is mishandling some part of the args
    (most likely the `from` address derivation in
    `_settleComplementaryMaker`/`_settleTakerOrder`)."""
    h1, _, _, _, ctf, _ = split_settled_with_delegate
    relay = probe_factory()

    from hashlib import sha256
    from dev.deals import deal_outcome, set_approval, ctf_balance, prepare_condition
    from dev.signing import bob as bob_signer_fn, carla as carla_signer_fn
    from dev.addrs import algod_addr_bytes_for_app

    bob_addr = bob_signer_fn().eth_address_padded32
    carla_addr = carla_signer_fn().eth_address_padded32

    # Use a fixed YES id (no need to compute canonical — CTFMock just
    # uses whatever id we pass for keying; the dance's `_validateTokenIds`
    # check doesn't fire since we're skipping matchOrders).
    YES = 0xA1A1A1A1A1A1A1A1
    h1_addr = algod_addr_bytes_for_app(h1.app_id)

    # Pre-populate carla's balance + carla→h1 approval. We're hitting
    # CTFMock.safeTransferFrom(carla, bob, YES, 100M) where the sender
    # at depth 3 is helper1, so we need approvals[carla, h1] = true.
    deal_outcome(ctf, carla_addr, YES, 100_000_000)
    set_approval(ctf, carla_addr, h1_addr, True)
    assert ctf_balance(ctf, carla_addr, YES) == 100_000_000

    # puya-sol's address convention for app contracts: 24 zero bytes +
    # 8-byte big-endian app_id. helper1's TEAL extracts the app_id via
    # `extract_uint64` at offset 24 of this 32-byte address.
    ctf_addr32 = b"\x00" * 24 + ctf.app_id.to_bytes(8, "big")

    yes_bytes = YES.to_bytes(32, "big")
    carla_bal_box = b"b_" + sha256(bytes(carla_addr) + yes_bytes).digest()
    bob_bal_box = b"b_" + sha256(bytes(bob_addr) + yes_bytes).digest()
    carla_ap_box = b"ap_" + sha256(bytes(carla_addr) + h1_addr).digest()

    relay.send.call(au.AppClientMethodCallParams(
        method="relayH1TransferFromERC1155",
        args=[h1.app_id, ctf_addr32, carla_addr, bob_addr, YES, 100_000_000],
        extra_fee=au.AlgoAmount(micro_algo=500_000),
        app_references=[h1.app_id, ctf.app_id],
        box_references=[
            au.BoxReference(app_id=ctf.app_id, name=carla_bal_box),
            au.BoxReference(app_id=ctf.app_id, name=bob_bal_box),
            au.BoxReference(app_id=ctf.app_id, name=carla_ap_box),
        ],
    ), send_params=AUTO_POPULATE)

    assert ctf_balance(ctf, carla_addr, YES) == 0, (
        "carla's YES wasn't debited — depth-3 helper1.TransferHelper path "
        "is broken even when bypassing matchOrders"
    )
    assert ctf_balance(ctf, bob_addr, YES) == 100_000_000


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
