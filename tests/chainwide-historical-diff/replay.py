#!/usr/bin/env python3
"""Orchestrator: fetch → EVM leg → AVM leg → diff, with symmetric re-skip.

  python3 replay.py <tag> [--host H --address A] [--max-txns N] [--fetch]

If the AVM leg hits a platform limit (opcode/box budget) on a txn, that txn is
added to the skip set and BOTH legs are re-run, so the two states stay in
lockstep instead of one leg forking.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import CASES, EVM_PY, HERE, load_json
from differ import diff_case, print_report


def _run(cmd, tag) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    for ln in out.splitlines():
        if ln.startswith(("[evm]", "[avm]")):
            print("  " + ln)
    if p.returncode != 0:
        print(f"  !! {tag} leg failed (rc={p.returncode}):")
        print("     " + "\n     ".join(out.strip().splitlines()[-6:]))
        raise SystemExit(1)
    return out


def replay(tag: str, max_txns: int = 300, snapshot_every: int = 25) -> dict:
    case_dir = CASES / tag
    skips: dict[int, str] = {}
    for attempt in range(1, 4):
        _run([str(EVM_PY), str(HERE / "evm_leg.py"), str(case_dir),
              json.dumps({"max_txns": max_txns, "snapshot_every": snapshot_every,
                          "pin_time": True,
                          "skips": {str(k): v for k, v in skips.items()}})], "evm")
        _run(["python3", str(HERE / "avm_leg.py"), str(case_dir),
              json.dumps({"skips": [str(k) for k in skips]})], "avm")
        pl = load_json(case_dir / "avm_results.json").get("platform_limits") or {}
        new = {int(k): f"avm-platform-limit:{v[:40]}" for k, v in pl.items()
               if int(k) not in skips}
        if not new:
            break
        skips.update(new)
        print(f"  ↻ re-running both legs with {len(new)} platform-limit skip(s)")
    return diff_case(case_dir)


def main():
    argv = list(sys.argv[1:])

    def opt(flag, default=None, cast=str):
        if flag in argv:
            i = argv.index(flag)
            v = cast(argv[i + 1]); del argv[i:i + 2]
            return v
        return default

    host = opt("--host")
    address = opt("--address")
    max_txns = opt("--max-txns", 300, int)
    snap = opt("--snapshot-every", 25, int)
    force_fetch = "--fetch" in argv
    if force_fetch:
        argv.remove("--fetch")
    if not argv:
        sys.exit(__doc__)
    tag = argv[0]

    if force_fetch or not (CASES / tag / "case.json").exists():
        if not (host and address):
            sys.exit(f"case {tag} not fetched — pass --host and --address")
        from fetch import fetch_case
        fetch_case(host, address, tag, max_txns)

    print(f"[replay] {tag}: max_txns={max_txns}")
    print_report(replay(tag, max_txns, snap))


if __name__ == "__main__":
    main()
