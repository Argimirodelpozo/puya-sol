#!/usr/bin/env python3
"""CI entry point for the chainwide diff on the oracle lane (no LocalNet).

Replays each tracked case through avm-prover's oracle (`oracle_case.py`), diffs
it against the recorded EVM leg (`differ.py`) and fails when any REAL divergence
bucket is non-zero or a case could not be replayed at all.

    python3 ci_oracle_lane.py [--prover-root DIR] [--oracle BIN] [tag ...]

Cases default to the ones whose inputs are tracked in git (see .gitignore):
selftest, pol, vanry (default storage model) and eul (slot mode; its EVM leg
was recorded in slot mode).
"""
from __future__ import annotations
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# tag -> extra oracle_case.py arguments (the compile mode must match the EVM run)
DEFAULT_CASES = {
    "selftest": [],
    "pol": [],
    "vanry": [],
    "eul": ["--evm-storage-layout"],
}
sys.path.insert(0, str(HERE))
from differ import _REAL_BUCKETS as REAL_BUCKETS  # noqa: E402  (the differ's own definition)


def run(cmd: list[str]) -> int:
    print("$", " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=HERE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--prover-root", default=os.environ.get("AVM_PROVER_ROOT"))
    parser.add_argument("--oracle", default=os.environ.get("AVM_ORACLE_BIN"))
    parser.add_argument("tags", nargs="*")
    args = parser.parse_args()
    tags = args.tags or list(DEFAULT_CASES)
    extra: list[str] = []
    if args.prover_root:
        extra += ["--prover-root", args.prover_root]
    if args.oracle:
        extra += ["--oracle", args.oracle]

    failures: list[str] = []
    summary: list[tuple[str, str]] = []
    for tag in tags:
        case_dir = HERE / "cases" / tag
        if not (case_dir / "case.json").exists():
            failures.append(f"{tag}: missing cases/{tag}/case.json")
            continue
        started = time.time()
        rc = run([sys.executable, "oracle_case.py", str(case_dir),
                  *DEFAULT_CASES.get(tag, []), *extra])
        if rc != 0:
            failures.append(f"{tag}: oracle_case.py exited {rc}")
            continue
        rc = run([sys.executable, "differ.py", str(case_dir)])
        if rc != 0:
            failures.append(f"{tag}: differ.py exited {rc}")
            continue
        report = json.loads((case_dir / "report.json").read_text())
        counts = report.get("counts") or {}
        real = {k: int(counts.get(k, 0) or 0) for k in REAL_BUCKETS}
        total_real = sum(real.values())
        replayed = report.get("replayed", "?")
        line = (f"{tag}: replayed={replayed} real={total_real} "
                f"({', '.join(f'{k}={v}' for k, v in real.items() if v)}) "
                f"{time.time() - started:.0f}s")
        summary.append((tag, line))
        if total_real:
            failures.append(line)
        elif not counts:
            failures.append(f"{tag}: report.json has no counts (keys: {sorted(report)[:8]})")

    print("\n== oracle lane summary")
    for _, line in summary:
        print("  ", line)
    if failures:
        print("\n== FAILURES")
        for f in failures:
            print("  ", f)
        return 1
    print("  ✅ no real divergences")
    return 0


if __name__ == "__main__":
    sys.exit(main())
