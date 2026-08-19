#!/usr/bin/env python3
"""Classify a joint replay's status mismatches into triage buckets.

  python3 triage_joint.py cctp_joint_report.json [--cases cases]

A deep window (3000+ txns/contract) reaches far past the hand-audited region
of MAINNET_RECEIPT_METADATA, so raw "N mismatches" is not a verdict — some of
it is indexer noise the campaign already understands. This sorts the rows so
the morning triage reads real signal first:

  budget_panic     AVM PANIC whose error names an inner-txn assert / opcode
                   budget — resource shape, not semantics. Re-runs with more
                   budget rather than a compiler fix.
  avm_rejects_ok   history says ok, AVM rejected: the class that contains real
                   compiler bugs. READ THESE FIRST.
  avm_accepts_bad  history says failed, AVM accepted: usually an unaudited
                   stale receipt (the verified corrections are exactly this
                   shape), but a permissive-check bug looks identical — the
                   zero-log heuristic below separates them.
  zero_log_ok      history says ok but the receipt carries NO logs, which is
                   self-contradictory for a state-changing CCTP method: the
                   established corrupt-indexer signature.
  dup_payload      a byte-identical payload appears more than once in the
                   window (relayer race; the driver reclassifies by outcome
                   multiset, this flags any that slipped through).

Nothing here decides anything on its own — it groups rows and prints the
evidence each bucket rests on.
"""
from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

# Deliberately NARROW. "assert failed pc=" and "logic eval error" are NOT
# budget signals — a program assert is semantics, and mislabelling one as a
# resource artifact buries exactly the rows worth reading (learned from the
# nonce-682 pair, whose pc mapped to `assert // Nonce already used`).
BUDGET_HINTS = ("opcode budget", "budget exceeded", "dynamic cost",
                "exceeds budget")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("report", type=Path)
    ap.add_argument("--cases", type=Path, default=Path(__file__).parent / "cases")
    ap.add_argument("--show", type=int, default=6, help="rows printed per bucket")
    args = ap.parse_args()

    rep = json.loads(args.report.read_text())
    results = rep["results"]
    summary = rep["summary"]

    logs_by_hash: dict[str, list] = {}
    for p in args.cases.glob("*/logs.json"):
        try:
            logs_by_hash.update(json.loads(p.read_text()))
        except Exception:
            pass

    # Duplicate detection keys on the PAYLOAD, not the method name: every
    # window repeats `receiveMessage`, so a signature-only key calls
    # everything a duplicate and says nothing.
    arg_key: dict[str, tuple] = {}
    for tag_dir in args.cases.glob("*/calls.json"):
        try:
            for c in json.loads(tag_dir.read_text())["calls"]:
                if c.get("sig"):
                    arg_key[c["hash"]] = (
                        c["sig"], json.dumps(c.get("args"), sort_keys=True))
        except Exception:
            pass
    payload_count = Counter(
        arg_key[r["hash"]] for r in results if r["hash"] in arg_key)

    buckets: dict[str, list] = defaultdict(list)
    for r in results:
        if r.get("matched_status"):
            continue
        err = (r.get("oracle_error") or "").lower()
        hist_ok = bool(r["historical_ok"])
        accepted = r["oracle_result"] == "ACCEPT"
        entries = logs_by_hash.get(r["hash"].split("#")[0])
        if hist_ok and entries is not None and len(entries) == 0:
            buckets["zero_log_ok"].append(r)
        elif r["oracle_result"] == "PANIC" and any(h in err for h in BUDGET_HINTS):
            buckets["budget_panic"].append(r)
        elif hist_ok and not accepted:
            buckets["avm_rejects_ok"].append(r)
        elif not hist_ok and accepted:
            buckets["avm_accepts_bad"].append(r)
        else:
            buckets["other"].append(r)
        if payload_count.get(arg_key.get(r["hash"]), 0) > 1:
            buckets["dup_payload"].append(r)

    print(f"{args.report.name}: {summary['compared_statuses']} compared, "
          f"{summary['matched_statuses']} matched, "
          f"{summary['status_mismatches']} mismatched")
    order = ["avm_rejects_ok", "avm_accepts_bad", "budget_panic",
             "zero_log_ok", "dup_payload", "other"]
    for name in order:
        rows = buckets.get(name) or []
        if not rows:
            continue
        print(f"\n── {name}: {len(rows)}")
        by_sig = Counter(r.get("signature") or "<no-sig>" for r in rows)
        for sig, n in by_sig.most_common(8):
            print(f"     {n:4d}  {sig}")
        for r in rows[:args.show]:
            print(f"   · {r['hash'][:18]} blk {r['block']} "
                  f"hist_ok={r['historical_ok']} {r['oracle_result']} "
                  f"{str(r.get('oracle_error') or '')[:110]}")
    if not any(buckets.values()):
        print("\nno unmatched rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
