#!/usr/bin/env python3
"""Measure the opcode budget the CCTP entry points actually need.

  python3 budget_probe.py --prover-root /path/to/avm-prover [--limit 120]

The joint replay hands every call the protocol's maximum pooled budget
(16 x 700 = 11,200 opcodes) plus OpUp escalation to a target, so a green run
proves only that nothing exceeded a generous ceiling. This bisects the target
until calls start failing, which bounds the real requirement:

    highest target that FAILS  <  requirement  <=  lowest target that PASSES

Run it after a deep-window round. If the requirement climbs between rounds,
something made the compiled code more expensive — a regression a plain
pass/fail replay cannot see, because the ceiling hides it until a real
deployment hits it.

Reference figures (v1, 120-call sample, 2026-08-19): 14000 passes, 12000
fails with 89 receiveMessage 'dynamic cost' panics => receiveMessage needs
12-14k opcodes, which is ABOVE the 11,200 pooled maximum, so OpUp is
mandatory rather than an optimisation.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
POOLED_MAX = 16 * 700


def run_at(target: int, cases: Path, prover: Path, limit: int,
           config: Path | None) -> tuple[int, int]:
    """(matched, compared) for one replay at this OpUp target."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out = Path(tmp.name)
    cmd = [sys.executable, str(HERE / "oracle_cctp_historical.py"), str(cases),
           "--prover-root", str(prover), "--limit", str(limit),
           "--continue-after-divergence", "--output", str(out)]
    if config:
        cmd += ["--config", str(config)]
    env = dict(os.environ, CCTP_ENSURE_BUDGET=str(target))
    subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=7200)
    try:
        rep = json.loads(out.read_text())
        s = rep["summary"]
        return s["matched_statuses"], s["compared_statuses"]
    except Exception:
        return -1, -1
    finally:
        out.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=Path, default=HERE / "cases")
    ap.add_argument("--prover-root", type=Path,
                    default=os.environ.get("AVM_PROVER_ROOT"), required=False)
    ap.add_argument("--config", type=Path)
    ap.add_argument("--limit", type=int, default=120)
    ap.add_argument("--lo", type=int, default=4000)
    ap.add_argument("--hi", type=int, default=45000)
    ap.add_argument("--tolerance", type=int, default=2000)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    if not args.prover_root:
        sys.exit("--prover-root (or AVM_PROVER_ROOT) is required")

    lo, hi = args.lo, args.hi          # lo assumed failing, hi assumed passing
    m, c = run_at(hi, args.cases, args.prover_root, args.limit, args.config)
    print(f"target {hi:6d} -> {m}/{c}")
    if c <= 0 or m != c:
        print("the upper bound does not pass — nothing to bisect")
        return 1
    trials = [{"target": hi, "matched": m, "compared": c, "pass": True}]
    while hi - lo > args.tolerance:
        mid = (lo + hi) // 2
        m, c = run_at(mid, args.cases, args.prover_root, args.limit, args.config)
        ok = c > 0 and m == c
        print(f"target {mid:6d} -> {m}/{c}  {'pass' if ok else 'FAIL'}")
        trials.append({"target": mid, "matched": m, "compared": c, "pass": ok})
        if ok:
            hi = mid
        else:
            lo = mid
    result = {
        "requirement_between": [lo, hi],
        "pooled_group_max": POOLED_MAX,
        "opup_mandatory": hi > POOLED_MAX,
        "sample_calls": args.limit,
        "trials": trials,
    }
    print(f"\nrequirement: >{lo} and <={hi} opcodes "
          f"(pooled max {POOLED_MAX}; "
          f"OpUp {'MANDATORY' if hi > POOLED_MAX else 'not required'})")
    if args.out:
        args.out.write_text(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
