#!/usr/bin/env python3
"""Run the historical replay across a list of candidate contracts, sequentially.

  python3 batch.py [--max-txns N] [--only tag1,tag2]

Sequential ON PURPOSE: one LocalNet, and concurrent puya-sol compiles poison the
shared compile cache. Failures (unverified / multi-file / old solc / external-
dependency constructor) are logged and skipped; the batch keeps going.
Aggregate results land in cases/_batch_summary.json.
"""
from __future__ import annotations

import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import CASES, dump_json
from differ import print_report
from fetch import fetch_case
from replay import replay

# (host, address, tag) — single-file verified ^0.8.x contracts with history.
CANDIDATES = [
    ("optimism.blockscout.com", "0x4200000000000000000000000000000000000042", "op_gov"),
    ("eth.blockscout.com",      "0x6982508145454Ce325dDbE47a25d4ec3d2311933", "pepe"),
    ("base.blockscout.com",     "0xCF205808Ed36593aa40a44F10c7f7C2F67d4A4d4", "friendtech"),
    ("base.blockscout.com",     "0xAC1Bd2486aAf3B5C0fc3Fd868558b082a531B2B4", "toshi"),
    ("eth.blockscout.com",      "0xaaeE1A9723aaDB7afA2810263653A34bA2C21C7a", "mog"),
    ("base.blockscout.com",     "0x4ed4E862860beD51a9570b96d89aF5E1B0Efefed", "degen"),
    ("eth.blockscout.com",      "0x163f8C2467924be0ae7B5347228CABF260318753", "wld"),
    ("gnosis.blockscout.com",   "0x177127622c4A00F3d409B75571e12cB3c8973d3c", "gno_cow"),
    # probed eligible: single-file, ^0.8.x, verified
    ("eth.blockscout.com",      "0xA35923162C49cF95e6BF26623385eb431ad920D3", "turbo"),
    ("eth.blockscout.com",      "0x72e4f9F808C49A2a61dE9C5896298920Dc4EEEa9", "bitcoin_hpos"),
    ("eth.blockscout.com",      "0x4d224452801ACEd8B2F0aebE155379bb5D594381", "ape"),
    ("eth.blockscout.com",      "0x5026F006B85729a8b14553FAE6af249aD16c9aaB", "kizuna"),
    ("base.blockscout.com",     "0x6921B130D297cc43754afba22e5EAc0FBf8Db75b", "doginme"),
]


def main():
    argv = list(sys.argv[1:])
    max_txns = 200
    only = None
    if "--max-txns" in argv:
        i = argv.index("--max-txns"); max_txns = int(argv[i + 1]); del argv[i:i + 2]
    if "--only" in argv:
        i = argv.index("--only"); only = set(argv[i + 1].split(",")); del argv[i:i + 2]
    refetch = "--refetch" in argv        # re-pull history (e.g. for a deeper window)
    if refetch:
        argv.remove("--refetch")

    summary = []
    for host, addr, tag in CANDIDATES:
        if only and tag not in only:
            continue
        print(f"\n{'='*70}\n[batch] {tag}  ({host} {addr})\n{'='*70}", flush=True)
        entry = {"tag": tag, "host": host, "address": addr}
        try:
            if refetch or not (CASES / tag / "case.json").exists():
                fetch_case(host, addr, tag, max_txns)
            rep = replay(tag, max_txns)
            print_report(rep)
            c = rep["counts"]
            entry.update({
                "status": "done", "name": rep["name"],
                "replayed": rep["replayed"], "window": rep["txns_in_window"],
                "skips": rep["skips"],
                "real_divergences": c["status_div"] + c["value_div"]
                                    + c["event_div"] + c["snapshot_div"],
                "counts": c,
            })
        except SystemExit as e:
            entry.update({"status": "skipped", "why": str(e)[:200]})
            print(f"[batch] {tag}: SKIPPED — {str(e)[:200]}", flush=True)
        except Exception as e:
            entry.update({"status": "error", "why": f"{type(e).__name__}: {e}"[:200]})
            print(f"[batch] {tag}: ERROR — {type(e).__name__}: {e}", flush=True)
            traceback.print_exc()
        summary.append(entry)
        dump_json(CASES / "_batch_summary.json", summary)

    print(f"\n{'='*70}\n[batch] SUMMARY\n{'='*70}")
    for e in summary:
        if e["status"] == "done":
            flag = "❌" if e["real_divergences"] else "✅"
            print(f"  {flag} {e['tag']:<12} {e.get('name','?'):<24} "
                  f"{e['replayed']}/{e['window']} replayed  "
                  f"divergences={e['real_divergences']}  skips={e['skips']}")
        else:
            print(f"  ·  {e['tag']:<12} {e['status'].upper()}: {e.get('why','')[:90]}")


if __name__ == "__main__":
    main()
