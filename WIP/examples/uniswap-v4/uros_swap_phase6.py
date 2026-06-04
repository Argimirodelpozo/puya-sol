#!/usr/bin/env python3
"""Phase 6: native + AERC20 V4 swap, per-currency flash accounting (#44).

A V4 pool whose currencies are c0 = NATIVE (ALGO) and c1 = a real AERC20. Seeded
with liquidity, then swapped zeroForOne (pay native, take AERC20 out) with REAL
movement on both legs. This is the native-side counterpart to phase8 (all-AERC20):
it exercises the native settle() path (a grouped payment) alongside per-currency
net-zero, which phase8 doesn't.

History: phase6 was originally a native swap whose c1 was a placeholder address and
whose output was ERC6909 claims, settled with one conflated native lump. The #44
per-currency hardening (debit==credit PER currency) correctly rejects that shortcut,
so phase6 is now a genuine native+AERC20 pool: the seed owes both currencies (split
from the per-currency scratch buckets), native settles via settle()+payment, the
AERC20 via settleCurrency()+axfer, and the swap output is a real AERC20 take.

    python WIP/examples/uniswap-v4/uros_swap_phase6.py
"""
# ruff: noqa: T201
from __future__ import annotations

import base64
import importlib.util as _ilu
import itertools
import json
import math
from pathlib import Path

import algokit_utils as au
import algosdk
from algosdk.transaction import (
    ApplicationCreateTxn,
    AssetTransferTxn,
    OnComplete,
    StateSchema,
    wait_for_confirmation,
)
from algosdk.v2client import models as _m

_patch = Path(__file__).resolve().parents[3] / "tests/solidity-semantic-tests/framework/_algosdk_patch.py"
_spec = _ilu.spec_from_file_location("_algosdk_int_patch", _patch)
_spec.loader.exec_module(_ilu.module_from_spec(_spec))

OUT = Path("/tmp/pm_full")
PMDIR = OUT / "PoolManager"
HELPERDIR = OUT / "PoolManager__Helper1"
AERC20_OUT = Path(__file__).resolve().parent.parent / "aerc20-demo/out"
WRITE_CHUNK = 2000
ZERO_ADDR = algosdk.encoding.encode_address(bytes(32))   # native currency c0
INIT_SQRT_PRICE = int(1.0001 ** 45 * (2 ** 96))          # ~tick 90, inside [60,120]
MINT = 500_000


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def cid(currency: str) -> int:
    """The 64-bit currency id the pool buckets flash deltas by (native => 0)."""
    return int.from_bytes(algosdk.encoding.decode_address(currency)[24:], "big")


def main() -> None:
    algorand = au.AlgorandClient.default_localnet()
    disp = algorand.account.localnet_dispenser()
    algorand.set_default_signer(disp.signer)
    sender = disp.address
    algod = algorand.client.algod

    # ── c1 = a real AERC20 (c0 is native, which sorts first as 0x00..00) ──
    ta = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.approval.teal").read_text())["result"])
    tc = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.clear.teal").read_text())["result"])
    tx = ApplicationCreateTxn(sender, algod.suggested_params(), OnComplete.NoOpOC, ta, tc, StateSchema(16, 16), StateSchema(0, 0))
    tok_app = wait_for_confirmation(algod, algod.send_transaction(tx.sign(disp.private_key)), 4)["application-index"]
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=algosdk.logic.get_application_address(tok_app), amount=au.AlgoAmount.from_algo(1)))
    tok = au.AppClient(au.AppClientParams(app_id=tok_app, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((AERC20_OUT / "MyToken.arc56.json").read_text()), default_sender=sender))
    tok.send.call(au.AppClientMethodCallParams(method="__postInit", args=[], extra_fee=au.AlgoAmount.from_micro_algo(2000)))
    asa1 = int(tok.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return)
    c1 = algosdk.encoding.encode_address(bytes(24) + tok_app.to_bytes(8, "big"))
    print(f"c0=native  c1=AERC20 app={tok_app} asa={asa1} currency={c1[:10]}..")

    # ── deploy V4 (setup + main + helper1 + opup + boost), load chunks, map ──
    manifest = json.loads((PMDIR / "deploy.uros.json").read_text())
    sel = {m["name"]: bytes.fromhex(m["selector"][2:]) for m in manifest["methods"]}
    mchunk = {m["name"]: m["chunk"] for m in manifest["methods"]}
    shell_program = (PMDIR / "PoolManager.approval.bin").read_bytes()
    clear_program = (PMDIR / "PoolManager.clear.bin").read_bytes()
    sch = json.loads((PMDIR / "PoolManager.arc56.json").read_text())["state"]["schema"]

    hf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((HELPERDIR / "PoolManager__Helper1.arc56.json").read_text()), default_sender=sender))
    helper_client, _ = hf.send.bare.create()
    helper1_id = helper_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=helper_client.app_address, amount=au.AlgoAmount.from_algo(1)))

    def chunk_bytes(name):
        teal = (PMDIR / f"PoolManager__chunk_{name}.approval.teal").read_text()
        if "TMPL_PoolManager__Helper1_APP_ID" in teal:
            teal = teal.replace("TMPL_PoolManager__Helper1_APP_ID", str(helper1_id))
            return base64.b64decode(algod.compile(teal)["result"])
        return (PMDIR / f"PoolManager__chunk_{name}.approval.bin").read_bytes()

    chunks = {n: chunk_bytes(n) for n in ("init", "modliq", "swap", "shell")}
    max_pages = max(math.ceil(len(p) / 2048) for p in chunks.values()) - 1

    opup_c = base64.b64decode(algod.compile("#pragma version 10\nint 1\nreturn\n")["result"])
    opup_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=opup_c, clear_state_program=opup_c)).app_id
    boost_src = ("#pragma version 10\ntxn ApplicationID\nbz ok\n"
                 "int 0\nloop:\ndup\nint 40\n<\nbz ok\nitxn_begin\nint 6\nitxn_field TypeEnum\n"
                 f"int {opup_id}\nitxn_field ApplicationID\nint 0\nitxn_field Fee\n"
                 "itxn_submit\nint 1\n+\nb loop\nok:\nint 1\nreturn\n")
    boost_c = base64.b64decode(algod.compile(boost_src)["result"])
    boost_id = algorand.send.app_create(au.AppCreateParams(sender=sender, approval_program=boost_c, clear_state_program=boost_c)).app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=algosdk.logic.get_application_address(boost_id), amount=au.AlgoAmount.from_algo(1)))

    sf = au.AppFactory(au.AppFactoryParams(algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "UrosSetup.arc56.json").read_text()), default_sender=sender))
    setup_client, _ = sf.send.bare.create()
    setup_id = setup_client.app_id
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=setup_client.app_address, amount=au.AlgoAmount.from_algo(25)))

    main_id = algorand.send.app_create(au.AppCreateParams(
        sender=sender, approval_program=shell_program, clear_state_program=clear_program, extra_program_pages=max_pages,
        schema={"global_ints": sch["global"]["ints"], "global_byte_slices": sch["global"]["bytes"],
                "local_ints": sch["local"]["ints"], "local_byte_slices": sch["local"]["bytes"]})).app_id
    main_addr = algosdk.logic.get_application_address(main_id)
    main_client = au.AppClient(au.AppClientParams(app_id=main_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((PMDIR / "PoolManager.arc56.json").read_text()), default_sender=sender))
    main_client.send.call(au.AppClientMethodCallParams(method="uros_set_setup", args=[setup_id]))
    setup_client.send.call(au.AppClientMethodCallParams(method="set_main", args=[main_id]))
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(5)))

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
    for name in ("initialize", "unlock", "modifyLiquidity", "swap", "settle", "settleCurrency", "take", "optInAsset"):
        s = sel[name]
        setup_client.send.call(au.AppClientMethodCallParams(method="map_method", args=[s, mchunk[name].encode()],
            box_references=[b"m" + s], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print(f"deployed V4: setup={setup_id} main={main_id} helper1={helper1_id}")

    _ctr = itertools.count(1)
    def nxt():
        return f"n{next(_ctr)}".encode()

    def prep(name):
        s = sel[name]
        return setup_client.params.call(au.AppClientMethodCallParams(method="prepare", args=[],
            app_references=[main_id], box_references=[b"m" + s, mchunk[name].encode(), b"clear", *empties(4)],
            note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(3000)))

    def add_boost(g, n):
        for k in range(n):
            g.add_app_call(au.AppCallParams(sender=sender, app_id=boost_id, note=f"b{k}".encode(),
                on_complete=algosdk.transaction.OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(45000)))

    def add_opup(g, n):
        for k in range(n):
            g.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"o{k}".encode(),
                on_complete=algosdk.transaction.OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(6000)))

    def unlock_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="unlock", args=[prep("unlock"), b""], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(2000))))

    def settle_native(g):  # c0: read the grouped payment at GroupIndex-2
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="settle", args=[prep("settle")], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(3000))))

    def settle_curr(g, currency):  # c1: read the grouped axfer at GroupIndex-2
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="settleCurrency", args=[prep("settleCurrency"), currency], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(3000))))

    def boxes_now():
        return [au.BoxReference(main_id, base64.b64decode(b["name"]))
                for b in algod.application_boxes(main_id).get("boxes", [])]

    # ── opt the pool into the AERC20 ASA + seed the user ──
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(1)))  # MBR
    main_client.send.call(au.AppClientMethodCallParams(method="optInAsset", args=[prep("optInAsset"), asa1],
        box_references=empties(4), asset_references=[asa1], static_fee=au.AlgoAmount.from_micro_algo(5000)))
    algod.send_transaction(AssetTransferTxn(sender, algod.suggested_params(), sender, 0, asa1).sign(disp.private_key))  # user opt-in
    tok.send.call(au.AppClientMethodCallParams(method="mint", args=[sender, MINT],
        extra_fee=au.AlgoAmount.from_micro_algo(2000)), send_params={"populate_app_call_resources": True})
    print(f"pool opted into asa1={asa1}; user holds {MINT} c1")

    pool_key = [ZERO_ADDR, c1, 3000, 60, ZERO_ADDR]

    # 1) initialize
    g = algorand.new_group()
    add_opup(g, 8)
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(method="initialize",
        args=[prep("initialize"), pool_key, INIT_SQRT_PRICE], app_references=[helper1_id],
        box_references=empties(6), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.send()
    print(f"pool initialized ✓  boxes={len(boxes_now())}")

    def _box_val(nm):
        return base64.b64decode(algod.application_box_by_name(main_id, nm)["value"])

    def find_slot0():
        target = INIT_SQRT_PRICE.to_bytes(20, "big")
        for b in algod.application_boxes(main_id).get("boxes", []):
            nm = base64.b64decode(b["name"]); idx = _box_val(nm).find(target)
            if idx >= 0:
                return nm, idx
        return None, None

    s0_name, s0_off = find_slot0()
    assert s0_name is not None, "could not locate slot0 sqrtPrice box after init"

    def read_sqrt():
        return int.from_bytes(_box_val(s0_name)[s0_off:s0_off + 20], "big")
    print(f"slot0 sqrtPriceX96 = {read_sqrt()} (= INIT {INIT_SQRT_PRICE})")

    def measure(build_ops, n_boost=2):
        g = algorand.new_group(); add_boost(g, n_boost); build_ops(g); g.build()
        req = _m.SimulateRequest(txn_groups=[], allow_unnamed_resources=True, extra_opcode_budget=320000,
            exec_trace_config=_m.SimulateTraceConfig(enable=True, scratch_change=True))
        grp = g._atc.simulate(algod, req).simulate_response["txn-groups"][0]
        fa = (grp.get("failed-at") or [len(grp["txn-results"]) - 1])[0]
        tr = grp["txn-results"][fa].get("exec-trace", {}).get("approval-program-trace", [])
        slots = {}
        for u in tr:
            for sc in u.get("scratch-changes", []):
                nv = sc.get("new-value", {})
                v = nv.get("uint", 0) if not nv.get("bytes") else int.from_bytes(base64.b64decode(nv["bytes"]), "big")
                slots[sc.get("slot")] = v
        buckets = {}
        for b in range(slots.get(6, 0)):
            buckets[slots.get(7 + 3 * b, 0)] = (slots.get(8 + 3 * b, 0), slots.get(9 + 3 * b, 0))
        return buckets, grp.get("failure-message")

    def modliq_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="modifyLiquidity", args=[prep("modifyLiquidity"), pool_key, [60, 120, 5_000_000, bytes(32)], b""],
            app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
            note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(8000))))

    # ── 2) SEED: modliq owes native c0 + AERC20 c1 (split per-currency) ──
    buckets, fm = measure(lambda g: (unlock_op(g), modliq_op(g)))
    owe0, owe1 = buckets.get(cid(ZERO_ADDR), (0, 0))[0], buckets.get(cid(c1), (0, 0))[0]
    print(f"=== SEED: modliq [60,120] owes native={owe0} c1={owe1} (buckets {buckets}) ===")
    assert owe0 > 0 and owe1 > 0, f"modliq should owe both currencies; got native={owe0} c1={owe1} (fm={fm})"
    gs = algorand.new_group(); add_boost(gs, 2); unlock_op(gs); modliq_op(gs)
    gs.add_payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_micro_algo(owe0)))
    settle_native(gs)
    gs.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=asa1, amount=owe1))
    settle_curr(gs, c1)
    gs.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    pool_c1 = int(tok.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[main_addr])).abi_return)
    print(f"  liquidity seeded ✓  pool holds c1={pool_c1}  boxes={len(boxes_now())}")
    assert pool_c1 == owe1, "pool should hold exactly the seeded AERC20"

    # ── 3) SWAP zeroForOne: pay native c0, take AERC20 c1 out (real take) ──
    SWAP_IN = 2000
    LIMIT_DOWN = int(1.0001 ** 31 * (2 ** 96))  # tick ~62 (price down)

    def swap_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="swap", args=[prep("swap"), pool_key, [True, -SWAP_IN, LIMIT_DOWN], b""],
            app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
            note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(500000))))

    buckets, fm = measure(lambda g: (unlock_op(g), swap_op(g)))
    in_amt = buckets.get(cid(ZERO_ADDR), (0, 0))[0]   # native debit
    out_amt = buckets.get(cid(c1), (0, 0))[1]         # c1 credit
    print(f"=== SWAP zeroForOne: pay native {in_amt}, take c1 {out_amt} (buckets {buckets}) ===")
    assert in_amt == SWAP_IN, f"exact-in must consume {SWAP_IN}, got {in_amt} (fm={fm})"
    assert SWAP_IN < out_amt < 2 * SWAP_IN, f"out {out_amt} out of sane range (fm={fm})"
    u_c1_before = int(tok.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[sender])).abi_return)
    gw = algorand.new_group(); add_boost(gw, 2); unlock_op(gw); swap_op(gw)
    gw.add_payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_micro_algo(in_amt)))
    settle_native(gw)
    gw.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
        method="take", args=[prep("take"), c1, sender, out_amt], app_references=[tok_app],
        asset_references=[asa1], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    gw.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    u_c1_after = int(tok.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[sender])).abi_return)
    assert u_c1_after - u_c1_before == out_amt, f"user should receive {out_amt} c1; got {u_c1_after - u_c1_before}"
    p_after = read_sqrt()
    assert p_after < INIT_SQRT_PRICE, f"zeroForOne must lower the pool price: {p_after} >= {INIT_SQRT_PRICE}"
    print(f"  swap ✓ paid native {in_amt}, received c1 {out_amt}; pool sqrtPrice {INIT_SQRT_PRICE} -> {p_after} (down)")
    print("\n✅ PHASE 6 PASS: native + AERC20 V4 swap with per-currency net-zero — native input "
          "settled via settle()+payment, AERC20 output taken via clawback, pool price moved, "
          "each currency netted to zero independently.")


if __name__ == "__main__":
    main()
