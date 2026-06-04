#!/usr/bin/env python3
"""Phase 6: V4 swap E2E attempt — seeds liquidity, then swaps zeroForOne.

Flow: initialize (~tick 90) -> seed modliq [60,120] + native settle (WORKS, boxes
1->3) -> swap zeroForOne exact-in, settle native input, take output as ERC6909
claims (mint).

STATUS (2026-06-04): seed works end-to-end. The swap COMPUTE now passes the
uint160 width checks (Node.h biguint->arc4.uintN trim landed this session), so it
runs Slot0/TickMath/SwapMath. BUT the swap is BLOCKED on the AVM's 256-inner-txn
cap: `Pool.swap`'s step-loop calls Helper1 tick-math (getSqrtPriceAtTick,
computeSwapStep, ...) via INNER TXNS (they must be Helper1-extracted — chunk_swap
is already 7885/8192 B, no room to inline them), and the loop hits EXACTLY 256
inner txns => it is not terminating (a SwapMath.computeSwapStep progression
miscompile; a tight sqrtPriceLimit does NOT bound the count, confirming the loop
itself doesn't progress). Next: trace computeSwapStep return values across loop
iterations on localnet to find the no-progress step. See memory uniswap-v4 notes.

NO compiler change beyond the (committed) biguint trim; loads the algosdk int{N}
patch. Run: python WIP/examples/uniswap-v4/uros_swap_phase6.py
"""
# ruff: noqa: T201
from __future__ import annotations

import base64
import importlib.util as _ilu
import json
import math
from pathlib import Path

import algokit_utils as au
import algosdk

_patch = Path(__file__).resolve().parents[3] / "tests/solidity-semantic-tests/framework/_algosdk_patch.py"
_spec = _ilu.spec_from_file_location("_algosdk_int_patch", _patch)
_spec.loader.exec_module(_ilu.module_from_spec(_spec))

OUT = Path("/tmp/pm_full")
PMDIR = OUT / "PoolManager"
HELPERDIR = OUT / "PoolManager__Helper1"
WRITE_CHUNK = 2000
ZERO_ADDR = algosdk.encoding.encode_address(bytes(32))
INIT_SQRT_PRICE = int(1.0001 ** 45 * (2 ** 96))  # ~tick 90, inside [60,120] seed range
MIN_SQRT = 4295128739  # V4 MIN_SQRT_RATIO


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def main() -> None:
    manifest = json.loads((PMDIR / "deploy.uros.json").read_text())
    sel = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in manifest["methods"]}
    mchunk = {m["name"]: m["chunk"] for m in manifest["methods"]}

    shell_program = (PMDIR / "PoolManager.approval.bin").read_bytes()
    clear_program = (PMDIR / "PoolManager.clear.bin").read_bytes()
    sch = json.loads((PMDIR / "PoolManager.arc56.json").read_text())["state"]["schema"]

    algorand = au.AlgorandClient.default_localnet()
    disp = algorand.account.localnet_dispenser()
    algorand.set_default_signer(disp.signer)
    sender = disp.address

    # Helper1 (sidecar tick-math)
    hf = au.AppFactory(au.AppFactoryParams(
        algorand=algorand,
        app_spec=au.Arc56Contract.from_json((HELPERDIR / "PoolManager__Helper1.arc56.json").read_text()),
        default_sender=sender))
    helper_client, _ = hf.send.bare.create()
    helper1_id = helper_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=helper_client.app_address, amount=au.AlgoAmount.from_algo(1)))

    def chunk_bytes(name: str) -> bytes:
        teal = (PMDIR / f"PoolManager__chunk_{name}.approval.teal").read_text()
        if "TMPL_PoolManager__Helper1_APP_ID" in teal:
            teal = teal.replace("TMPL_PoolManager__Helper1_APP_ID", str(helper1_id))
            return base64.b64decode(algorand.client.algod.compile(teal)["result"])
        return (PMDIR / f"PoolManager__chunk_{name}.approval.bin").read_bytes()

    chunks = {n: chunk_bytes(n) for n in ("init", "modliq", "swap", "shell")}
    max_pages = max(math.ceil(len(p) / 2048) for p in chunks.values()) - 1

    # opup budget app (int 1)
    opup_c = base64.b64decode(algorand.client.algod.compile("#pragma version 10\nint 1\nreturn\n")["result"])
    opup_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=opup_c, clear_state_program=opup_c)).app_id

    # setup + main
    sf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "UrosSetup.arc56.json").read_text()), default_sender=sender))
    setup_client, _ = sf.send.bare.create()
    setup_id = setup_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(25)))

    main_id = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell_program, clear_state_program=clear_program, extra_program_pages=max_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]})).app_id
    main_client = au.AppClient(au.AppClientParams(app_id=main_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "PoolManager.arc56.json").read_text()), default_sender=sender))
    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_id]))
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_algo(5)))

    # load codeboxes + map methods
    boxes = [(b"clear", clear_program)] + [(n.encode(), chunks[n]) for n in chunks]
    i = 0
    while i < len(boxes):
        batch, total = [], 0
        while i < len(boxes) and len(batch) < 6 and (not batch or total + len(boxes[i][1]) < 4096 * 3):
            batch.append(boxes[i]); total += len(boxes[i][1]); i += 1
        setup_client.send.call(au.AppClientMethodCallParams(method="create_codeboxes",
            args=[[(k, len(d)) for k, d in batch]], box_references=[k for k, _ in batch],
            static_fee=au.AlgoAmount.from_micro_algo(8000)), send_params={"populate_app_call_resources": True})
    for key, data in boxes:
        for off in range(0, len(data), WRITE_CHUNK):
            setup_client.send.call(au.AppClientMethodCallParams(method="write_box", args=[key, off, data[off:off + WRITE_CHUNK]],
                box_references=[key], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    for name in ("initialize", "unlock", "modifyLiquidity", "swap", "settle", "mint"):
        s = sel[name]
        setup_client.send.call(au.AppClientMethodCallParams(method="map_method", args=[s, mchunk[name].encode()],
            box_references=[b"m" + s], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print(f"deployed: setup={setup_id} main={main_id} helper1={helper1_id} opup={opup_id}")

    def prep(name: str):
        s = sel[name]
        return setup_client.params.call(au.AppClientMethodCallParams(method="prepare", args=[],
            app_references=[main_id], box_references=[b"m" + s, mchunk[name].encode(), b"clear", *empties(4)],
            static_fee=au.AlgoAmount.from_micro_algo(3000)))

    pool_key = [ZERO_ADDR, sender, 3000, 60, ZERO_ADDR]  # c0<c1, fee, tickSpacing, no hooks

    # 1) initialize the pool
    g = algorand.new_group()
    for k in range(8):
        g.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"o{k}".encode(),
            on_complete=algosdk.transaction.OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(1000)))
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(method="initialize",
        args=[prep("initialize"), pool_key, INIT_SQRT_PRICE],
        app_references=[helper1_id], box_references=empties(6), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.send()
    print("pool initialized ✓")

    # the pool state box main wrote in initialize — modliq must reference it to find
    # the pool (populate didn't add it for the reverting group), else PoolNotInitialized.
    pool_boxes = [au.BoxReference(main_id, base64.b64decode(b["name"]))
                  for b in algorand.client.algod.application_boxes(main_id).get("boxes", [])]
    print(f"pool state boxes on main: {len(pool_boxes)}")

    # ── helpers ──────────────────────────────────────────────────────────
    from algosdk.v2client import models as _m

    def add_opup(g, n):
        for k in range(n):
            g.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"o{k}".encode(),
                on_complete=algosdk.transaction.OnComplete.NoOpOC,
                static_fee=au.AlgoAmount.from_micro_algo(6000)))

    def unlock_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="unlock", args=[prep("unlock"), b""], box_references=empties(4),
            static_fee=au.AlgoAmount.from_micro_algo(2000))))

    def boxes_now():
        return [au.BoxReference(main_id, base64.b64decode(b["name"]))
                for b in algorand.client.algod.application_boxes(main_id).get("boxes", [])]

    def measure(build_ops, n_opup=8):
        # simulate the ops alone (no settle) -> reverts CurrencyNotSettled; read
        # the conflated DEBIT(6)/CREDIT(7) the op wrote in its own txn scratch.
        g = algorand.new_group(); add_opup(g, n_opup); build_ops(g); g.build()
        req = _m.SimulateRequest(txn_groups=[], allow_unnamed_resources=True, extra_opcode_budget=160000,
            exec_trace_config=_m.SimulateTraceConfig(enable=True, scratch_change=True))
        grp = g._atc.simulate(algorand.client.algod, req).simulate_response["txn-groups"][0]
        fa = (grp.get("failed-at") or [len(grp["txn-results"]) - 1])[0]
        tr = grp["txn-results"][fa].get("exec-trace", {}).get("approval-program-trace", [])
        d = c = 0
        for u in tr:
            for sc in u.get("scratch-changes", []):
                nv = sc.get("new-value", {}); v = nv.get("uint", 0) if not nv.get("bytes") else int.from_bytes(base64.b64decode(nv["bytes"]), "big")
                if sc.get("slot") == 6: d = v
                if sc.get("slot") == 7: c = v
        return d, c, grp.get("failure-message")

    # ── 2) SEED liquidity: modliq [60,120] (spans current tick ~90) + native settle ──
    def modliq_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="modifyLiquidity", args=[prep("modifyLiquidity"), pool_key, [60, 120, 5_000_000, bytes(32)], b""],
            app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
            static_fee=au.AlgoAmount.from_micro_algo(8000))))
    seed_owed, _c, fm = measure(lambda g: (unlock_op(g), modliq_op(g)))
    print(f"=== SEED: modliq [60,120] owes {seed_owed} (fm={fm}) ===")
    gs = algorand.new_group(); add_opup(gs, 7); unlock_op(gs); modliq_op(gs)
    gs.add_payment(au.PaymentParams(sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_micro_algo(seed_owed)))
    gs.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="settle", args=[prep("settle")], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(3000))))
    gs.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    print(f"  liquidity seeded \u2713  pool boxes now = {len(boxes_now())}")

    # ── 3) SWAP zeroForOne exact-input: pay native c0, take c1 output as claims (mint) ──
    SWAP_IN = 2000
    # sqrtPriceLimit kept INSIDE the seeded range [60,120]: ~tick 62 (current ~90),
    # so a zeroForOne swap moves price down a bounded amount and never crosses tick
    # 60 into the empty range — which would walk the whole tick bitmap (one inner
    # Helper1 call per word → "too many inner transactions").
    SWAP_LIMIT = int(1.0001 ** 31 * (2 ** 96))  # sqrtRatio(tick 62)
    def swap_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="swap", args=[prep("swap"), pool_key, [True, -SWAP_IN, SWAP_LIMIT], b""],
            app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
            static_fee=au.AlgoAmount.from_micro_algo(500000))))
    amtIn, amtOut, fm = measure(lambda g: (unlock_op(g), swap_op(g)))
    print(f"=== SWAP zeroForOne exact-in {SWAP_IN}: DEBIT amountIn={amtIn} CREDIT amountOut={amtOut} (fm={fm}) ===")
    if amtIn == 0 or amtOut == 0:
        # DOCUMENTED FRONTIER (2026-06-04): the swap COMPUTE runs (uint160 width
        # checks pass after the biguint trim) but Pool.swap's step-loop hits the
        # AVM 256-inner-txn cap — getSqrtPriceAtTick/computeSwapStep are Helper1
        # inner txns (chunk_swap is full) and the loop does not terminate. Seed
        # liquidity above works E2E; this is the remaining swap blocker.
        print("⚠️  SWAP E2E BLOCKED at the 256-inner-txn cap (non-terminating "
              "Pool.swap loop). Seed liquidity works; swap is the documented frontier.")
        return
    gw = algorand.new_group(); add_opup(gw, 5); unlock_op(gw); swap_op(gw)
    gw.add_payment(au.PaymentParams(sender=sender, receiver=main_client.app_address, amount=au.AlgoAmount.from_micro_algo(amtIn)))
    gw.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="settle", args=[prep("settle")], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(3000))))
    gw.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="mint", args=[prep("mint"), sender, 1, amtOut], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(3000))))
    gw.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    print("\n\u2705 PASS: V4 swap completed E2E on the AVM \u2014 swap math + delta accounting, "
          "native input settled, output taken as ERC6909 claims, atomic-group net-zero passed.")


if __name__ == "__main__":
    main()
