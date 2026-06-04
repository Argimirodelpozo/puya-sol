#!/usr/bin/env python3
"""Phase 8: bidirectional V4 swap with TWO real AERC20 currencies (#53 / #44).

Builds on phase6 (native swap) + phase7 (AERC20 flash). Two distinct AERC20s become
the pool's currencies c0<c1; the pool opts into both ASAs, is seeded liquidity (modliq
owes BOTH c0 and c1), then swaps both ways with REAL token movement:
  * zeroForOne: pay c0 (axfer + settle), take c1 out (real AERC20 clawback take)
  * oneForZero: pay c1, take c0 out

The conflated flash slots (6=DEBIT / 7=CREDIT, currency-agnostic) suffice for the
HAPPY path because each currency's debit/credit sum still matches across the group
(swap debits c0/credits c1; settle credits c0; take debits c1 => totals equal). The
one wrinkle is MEASURING the seed: modliq debits BOTH c0+c1 into slot 6, so we read
the ordered slot-6 trace steps to split amount0 (first step) vs amount1 (delta). The
per-currency net-zero hardening (so a cross-currency cheat can't pass) is #44, a
separate security pass; this phase proves the mechanism works.

    python WIP/examples/uniswap-v4/uros_bidir_phase8.py
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
ZERO_ADDR = algosdk.encoding.encode_address(bytes(32))
INIT_SQRT_PRICE = int(1.0001 ** 45 * (2 ** 96))  # ~tick 90, inside [60,120]
MINT = 500_000  # AERC20 minted to the user for seeding/swapping (MyToken total is 1M)


def empties(n: int) -> list[au.BoxReference]:
    return [au.BoxReference(0, b"") for _ in range(n)]


def cid(currency: str) -> int:
    """The 64-bit currency id the pool buckets flash deltas by: the low 8 bytes of the
    address (an AERC20's controlling-app id; native address(0) => 0)."""
    return int.from_bytes(algosdk.encoding.decode_address(currency)[24:], "big")


def deploy_aerc20(algorand, sender, label):
    """Deploy a MyToken AERC20 instance; return (app_id, asa_id, currency_addr)."""
    algod = algorand.client.algod
    ta = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.approval.teal").read_text())["result"])
    tc = base64.b64decode(algod.compile((AERC20_OUT / "MyToken.clear.teal").read_text())["result"])
    tx = ApplicationCreateTxn(sender, algod.suggested_params(), OnComplete.NoOpOC, ta, tc,
                              StateSchema(16, 16), StateSchema(0, 0))
    app_id = wait_for_confirmation(algod, algod.send_transaction(tx.sign(_sk(algorand, sender))), 4)["application-index"]
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=algosdk.logic.get_application_address(app_id),
                                           amount=au.AlgoAmount.from_algo(1)))
    tok = au.AppClient(au.AppClientParams(app_id=app_id, algorand=algorand,
        app_spec=au.Arc56Contract.from_json((AERC20_OUT / "MyToken.arc56.json").read_text()), default_sender=sender))
    tok.send.call(au.AppClientMethodCallParams(method="__postInit", args=[], extra_fee=au.AlgoAmount.from_micro_algo(2000)))
    asa = int(tok.send.call(au.AppClientMethodCallParams(method="asaId")).abi_return)
    currency = algosdk.encoding.encode_address(bytes(24) + app_id.to_bytes(8, "big"))
    print(f"  AERC20 {label}: app={app_id} asa={asa} currency={currency[:10]}..")
    return app_id, asa, currency, tok


def _sk(algorand, addr):
    return algorand.account.localnet_dispenser().private_key


def main() -> None:
    algorand = au.AlgorandClient.default_localnet()
    disp = algorand.account.localnet_dispenser()
    algorand.set_default_signer(disp.signer)
    sender = disp.address
    algod = algorand.client.algod

    # ── two AERC20 currencies, sorted c0 < c1 by their app-id-encoded address ──
    a = deploy_aerc20(algorand, sender, "A")
    b = deploy_aerc20(algorand, sender, "B")
    (tok0_app, asa0, c0, tok0), (tok1_app, asa1, c1, tok1) = sorted([a[:3] + (a[3],), b[:3] + (b[3],)], key=lambda t: t[2])
    print(f"sorted: c0={c0[:10]}.. (app {tok0_app}) c1={c1[:10]}.. (app {tok1_app})")

    # ── deploy V4 (setup + main + helper1 + opup + boost), load chunks, map methods ──
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
    for name in ("initialize", "unlock", "modifyLiquidity", "swap", "settleCurrency", "take", "optInAsset"):
        s = sel[name]
        setup_client.send.call(au.AppClientMethodCallParams(method="map_method", args=[s, mchunk[name].encode()],
            box_references=[b"m" + s], static_fee=au.AlgoAmount.from_micro_algo(2000)))
    print(f"deployed V4: setup={setup_id} main={main_id} helper1={helper1_id}")

    # unique-note nonce: settle (and its prepare) are issued twice per group, so
    # without distinct notes they'd be byte-identical txns => "already in ledger".
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

    def boxes_now():
        return [au.BoxReference(main_id, base64.b64decode(bx["name"]))
                for bx in algod.application_boxes(main_id).get("boxes", [])]

    # ── opt the pool into both ASAs (creator-guarded; sender is creator) + seed the user ──
    algorand.send.payment(au.PaymentParams(sender=sender, receiver=main_addr, amount=au.AlgoAmount.from_algo(1)))  # MBR
    for asa in (asa0, asa1):
        main_client.send.call(au.AppClientMethodCallParams(method="optInAsset", args=[prep("optInAsset"), asa],
            box_references=empties(4), asset_references=[asa], static_fee=au.AlgoAmount.from_micro_algo(5000)))
    for tok, asa in ((tok0, asa0), (tok1, asa1)):
        algod.send_transaction(AssetTransferTxn(sender, algod.suggested_params(), sender, 0, asa).sign(disp.private_key))  # user opt-in
        tok.send.call(au.AppClientMethodCallParams(method="mint", args=[sender, MINT],
            extra_fee=au.AlgoAmount.from_micro_algo(2000)), send_params={"populate_app_call_resources": True})
    print(f"pool opted into asa0={asa0} asa1={asa1}; user holds {MINT} of each")

    pool_key = [c0, c1, 3000, 60, ZERO_ADDR]

    # 1) initialize
    g = algorand.new_group()
    add_opup(g, 8)
    g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(method="initialize",
        args=[prep("initialize"), pool_key, INIT_SQRT_PRICE], app_references=[helper1_id],
        box_references=empties(6), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.send()
    print(f"pool initialized ✓  boxes={len(boxes_now())}")

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
        # reconstruct the per-currency buckets: count@6, then {currId,debit,credit} at 7+3b
        buckets = {}  # currId -> (debit, credit)
        for b in range(slots.get(6, 0)):
            buckets[slots.get(7 + 3 * b, 0)] = (slots.get(8 + 3 * b, 0), slots.get(9 + 3 * b, 0))
        return buckets, grp.get("failure-message")

    def modliq_op(g):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="modifyLiquidity", args=[prep("modifyLiquidity"), pool_key, [60, 120, 5_000_000, bytes(32)], b""],
            app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
            note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(8000))))

    def settle_op(g, currency):
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="settleCurrency", args=[prep("settleCurrency"), currency], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(3000))))

    # ── 2) SEED: modliq owes BOTH c0 and c1; split the conflated DEBIT trace steps ──
    buckets, fm = measure(lambda g: (unlock_op(g), modliq_op(g)))
    owe0, owe1 = buckets.get(cid(c0), (0, 0))[0], buckets.get(cid(c1), (0, 0))[0]
    assert owe0 > 0 and owe1 > 0, f"modliq should owe both currencies; got c0={owe0} c1={owe1} (fm={fm})"
    print(f"=== SEED: modliq [60,120] owes c0={owe0} c1={owe1} (per-currency buckets {buckets}) ===")
    gs = algorand.new_group(); add_boost(gs, 2); unlock_op(gs); modliq_op(gs)
    gs.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=asa0, amount=owe0))
    settle_op(gs, c0)
    gs.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=asa1, amount=owe1))
    settle_op(gs, c1)
    gs.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    pool_c0 = int(tok0.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[main_addr])).abi_return)
    pool_c1 = int(tok1.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[main_addr])).abi_return)
    print(f"  liquidity seeded ✓  pool holds c0={pool_c0} c1={pool_c1}  boxes={len(boxes_now())}")
    assert pool_c0 == owe0 and pool_c1 == owe1, "pool should hold exactly the seeded amounts"

    # ── 3) BIDIRECTIONAL SWAP: zeroForOne (pay c0, take c1) then oneForZero (pay c1, take c0) ──
    SWAP_IN = 2000
    LIMIT_DOWN = int(1.0001 ** 31 * (2 ** 96))   # tick ~62 (zeroForOne moves price DOWN)
    LIMIT_UP = int(1.0001 ** 59 * (2 ** 96))     # tick ~118 (oneForZero moves price UP)

    def bal(tok):
        return int(tok.send.call(au.AppClientMethodCallParams(method="balanceOf", args=[sender])).abi_return)

    def do_swap(zfo, in_asa, in_currency, in_tok, out_currency, out_app, out_asa, out_tok, limit, label):
        def swap_op(g):
            g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
                method="swap", args=[prep("swap"), pool_key, [zfo, -SWAP_IN, limit], b""],
                app_references=[helper1_id], box_references=[*boxes_now(), *empties(6 - len(boxes_now()))],
                note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(500000))))
        buckets, fm = measure(lambda g: (unlock_op(g), swap_op(g)))
        in_amt = buckets.get(cid(in_currency), (0, 0))[0]    # input currency: debit
        out_amt = buckets.get(cid(out_currency), (0, 0))[1]  # output currency: credit
        print(f"=== SWAP {label}: in={in_amt} out={out_amt} (fm={fm}) ===")
        assert in_amt == SWAP_IN, f"exact-in must consume {SWAP_IN}, got {in_amt} (fm={fm})"
        assert SWAP_IN // 2 < out_amt < SWAP_IN * 2, f"out {out_amt} out of sane range (fm={fm})"
        ib, ob = bal(in_tok), bal(out_tok)
        g = algorand.new_group(); add_boost(g, 2); unlock_op(g); swap_op(g)
        g.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=in_asa, amount=in_amt))
        settle_op(g, in_currency)
        g.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="take", args=[prep("take"), out_currency, sender, out_amt], app_references=[out_app],
            asset_references=[out_asa], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(6000))))
        g.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
        assert ib - bal(in_tok) == in_amt, f"{label}: user should pay exactly {in_amt}"
        assert bal(out_tok) - ob == out_amt, f"{label}: user should receive exactly {out_amt}"
        print(f"  {label} ✓ paid {in_amt}, received {out_amt} — real AERC20 on both legs")

    do_swap(True, asa0, c0, tok0, c1, tok1_app, asa1, tok1, LIMIT_DOWN, "zeroForOne (c0->c1)")
    do_swap(False, asa1, c1, tok1, c0, tok0_app, asa0, tok0, LIMIT_UP, "oneForZero (c1->c0)")

    # ── SECURITY: a CROSS-CURRENCY CHEAT must revert under per-currency net-zero (#44).
    #    Take c1 out but "settle" with c0 — equal amounts, so the OLD conflated model
    #    (sum across currencies) would have passed it, letting the user walk off with c1.
    #    Per-currency requires c1's own debit==credit, so this reverts and the take rolls back.
    cheated = False
    try:
        gx = algorand.new_group(); add_boost(gx, 2); unlock_op(gx)
        gx.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(
            method="take", args=[prep("take"), c1, sender, 100], app_references=[tok1_app],
            asset_references=[asa1], box_references=empties(4), note=nxt(), static_fee=au.AlgoAmount.from_micro_algo(6000))))
        gx.add_asset_transfer(au.AssetTransferParams(sender=sender, receiver=main_addr, asset_id=asa0, amount=100))  # pay c0 — WRONG currency
        settle_op(gx, c0)
        gx.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
    except Exception as e:  # noqa: BLE001
        cheated = any(s in str(e) for s in ("assert", "logic eval", "CurrencyNotSettled", "err opcode"))
    assert cheated, "SECURITY: cross-currency cheat (take c1, settle c0) MUST revert under per-currency net-zero"
    print("✅ cross-currency cheat (take c1 / settle c0) correctly REVERTED — per-currency net-zero holds")

    print("\n✅ PHASE 8 PASS: bidirectional V4 swap with TWO real AERC20 currencies — both legs "
          "moved real tokens (settle the input axfer, take the output via clawback), per-currency "
          "amounts split from the conflated flash, atomic net-zero held on every group.")


if __name__ == "__main__":
    main()
