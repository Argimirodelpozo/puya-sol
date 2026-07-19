#!/usr/bin/env python3
"""Differential-fuzz REAL-WORLD contracts (WIP/examples) against live solc+EVM.

The mutation/generator corpus is the Solidity isoltest fixtures — small + targeted.
This points the stateful differential oracle at self-contained real contracts
(ERC20s, staking, governance, timelocks, DAOs…), fuzzing random call sequences vs
EVM ground truth — coverage their fixed-vector suites never exercise.

Usage: python fuzz_realworld.py <list-file> [--max-per-fn N] [--start K] [--limit M]
  <list-file>: one .sol path per line (absolute or repo-relative).
"""
import sys
import time
from pathlib import Path

from fuzz_evm import HERE
from fuzz_state import run_stateful_diff
from framework import Harness
from framework.localnet import LocalNet


def main():
    argv = sys.argv[1:]
    def opt(flag, d, cast=int):
        return cast(argv[argv.index(flag) + 1]) if flag in argv else d
    max_per_fn = opt("--max-per-fn", 8)
    start = opt("--start", 0)
    limit = opt("--limit", 10**9)
    paths = [Path(l.strip()).resolve() for l in open(argv[0]) if l.strip() and not l.startswith("#")]
    paths = paths[start:start + limit]

    ln = LocalNet(); h = Harness(ln, HERE / "out_realworld")
    stats = {"clean": 0, "compile_fail": 0, "deploy_fail": 0, "no_fns": 0,
             "value_div": 0, "event_div": 0, "revert_div": 0, "error": 0}
    findings = []
    for i, p in enumerate(paths):
        tag = f"{p.parent.parent.name}/{p.name}"
        try:
            res = run_stateful_diff(p, max_per_fn=max_per_fn, harness=h, quiet=True)
        except SystemExit as e:                          # oracle: solc reject / no fuzzable fns
            m = str(e).lower()
            stats["no_fns" if "fuzzable" in m else "compile_fail"] += 1
            continue
        except Exception as e:
            msg = str(e)[:120]
            low = msg.lower()
            if "deploy" in low or "create txn" in low or "logic eval" in low:
                stats["deploy_fail"] += 1
            elif "compile" in low:
                stats["compile_fail"] += 1
            else:
                stats["error"] += 1
                print(f"  ⚠️  {tag}: {type(e).__name__}: {msg}")
            continue
        vd, ed, rd = res["diverged"], res["event_div"], res["revert_div"]
        if vd or ed or rd:
            kinds = []
            if vd: kinds.append(f"{len(vd)} value"); stats["value_div"] += 1
            if ed: kinds.append(f"{len(ed)} event"); stats["event_div"] += 1
            if rd: kinds.append(f"{len(rd)} revert"); stats["revert_div"] += 1
            findings.append((tag, res))
            print(f"  ❌ {tag}: {', '.join(kinds)} divergence(s)  [{res['n_calls']} calls]")
            for sig, args, exp, act in vd[:4]:
                print(f"       {sig}{args}  evm={exp}  avm={act}")
            for sig, args, ev, av in rd[:3]:
                print(f"       revert {sig}{args}  evm={ev}  avm={av}")
        else:
            stats["clean"] += 1
            print(f"  ✅ {tag}  ({res['n_calls']} calls, {len(res['evm_skips'])} skips)")

    print("\n" + "=" * 70)
    print(" | ".join(f"{k}={v}" for k, v in stats.items()))
    print(f"REAL-WORLD DIVERGENCES: {len(findings)}")
    for tag, res in findings:
        print(f"  ❌ {tag}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
