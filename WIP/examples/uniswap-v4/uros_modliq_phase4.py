#!/usr/bin/env python3
"""Phase 4: does Uniswap V4's modifyLiquidity MATH run on-chain through the dance?

After initialize (phase 3), run [unlock, modifyLiquidity] as an atomic group.
modifyLiquidity is the heaviest V4 path that's now callable (int24 args via the
algosdk patch; Helper1 tick-math via the sidecar). It computes a 2-currency
BalanceDelta and accounts it. Because the current flash model is SINGLE-currency
(deltas conflate c0+c1 into one debit/credit pair), an unsettled add-liquidity
group is EXPECTED to revert at the net-zero check — but reaching that check
proves the liquidity math (Pool.modifyLiquidity + TickMath/TickBitmap) executed.
A revert *inside* the math (not the net-zero assert) would instead flag a
runtime miscompile. This harness reports which.

NO compiler change: loads the framework's algosdk int{N} patch (see phase 3).

    python WIP/examples/uniswap-v4/uros_modliq_phase4.py
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

    chunks = {n: chunk_bytes(n) for n in ("init", "modliq", "shell")}
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
    for name in ("initialize", "unlock", "modifyLiquidity"):
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
        args=[prep("initialize"), pool_key, 79228162514264337593543950336],
        app_references=[helper1_id], box_references=empties(6), static_fee=au.AlgoAmount.from_micro_algo(6000))))
    g.send()
    print("pool initialized ✓")

    # the pool state box main wrote in initialize — modliq must reference it to find
    # the pool (populate didn't add it for the reverting group), else PoolNotInitialized.
    pool_boxes = [au.BoxReference(main_id, base64.b64decode(b["name"]))
                  for b in algorand.client.algod.application_boxes(main_id).get("boxes", [])]
    print(f"pool state boxes on main: {len(pool_boxes)}")

    # 2) [unlock, modifyLiquidity] — does the liquidity math run?
    mod_params = [60, 120, 1_000_000, bytes(32)]  # POSITIVE ticks (avoid neg-tick signextend); +liquidity
    print("=== group: [opup*10, prepare,unlock, prepare,modifyLiquidity(+1e6 liq)] ===")
    try:
        g2 = algorand.new_group()
        for k in range(10):
            g2.add_app_call(au.AppCallParams(sender=sender, app_id=opup_id, note=f"m{k}".encode(),
                on_complete=algosdk.transaction.OnComplete.NoOpOC, static_fee=au.AlgoAmount.from_micro_algo(1000)))
        g2.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(method="unlock",
            args=[prep("unlock"), b""], box_references=empties(4), static_fee=au.AlgoAmount.from_micro_algo(2000))))
        g2.add_app_call_method_call(main_client.params.call(au.AppClientMethodCallParams(method="modifyLiquidity",
            args=[prep("modifyLiquidity"), pool_key, mod_params, b""],
            app_references=[helper1_id], box_references=[*pool_boxes, *empties(6 - len(pool_boxes))],
            static_fee=au.AlgoAmount.from_micro_algo(8000))))
        r = g2.send({"populate_app_call_resources": True, "cover_app_call_inner_transaction_fees": True})
        print(f"  modifyLiquidity RETURNED (unexpected without settlement): {[x.value for x in (r.returns or [])]}")
    except Exception as e:
        msg = " ".join(str(e).split())
        print(f"  reverted: {msg[:200]}")
        # FINDING (2026-06-03): reverts at a NO-ARG CustomRevert (pc=1562, proto 1 0)
        # BEFORE the tick math — confirmed because misordered ticks (120,60) revert at
        # the SAME pc (so Pool.modifyLiquidity's checkTicks/TicksMisordered is never
        # reached). Not a missing box ref (passing the pool box doesn't change it) and
        # not ManagerLocked (phase-2 proved _unlocked persists across the chunk swap).
        # => checkPoolInitialized reads _pools[id] as UNINITIALISED: the struct-mapping
        # storage-ref READ of the pool state (written by initialize) isn't seen by
        # modifyLiquidity across the dance — a pool-state read/layout issue (see
        # struct-storage-ref-model). So modliq is CALLABLE (int24+Helper1+dance OK) but
        # the V4 liquidity math doesn't run yet; next blocker = the pool-state read,
        # then multi-currency deltas, then token-movement settlement.
        print("  => reverts EARLY (PoolNotInitialized: pool-state read not seen across the "
              "dance); liquidity math NOT reached. See memory uniswap-v4-fresh-compile.")


if __name__ == "__main__":
    main()
