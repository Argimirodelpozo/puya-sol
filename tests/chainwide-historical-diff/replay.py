#!/usr/bin/env python3
"""Orchestrator: fetch → EVM leg → AVM leg → diff, with symmetric re-skip.

  python3 replay.py <tag> [--host H --address A] [--max-txns N] [--fetch]
                    [--evm-storage-layout]

If the AVM leg hits a platform limit (opcode/box budget) on a txn, that txn is
added to the skip set and BOTH legs are re-run, so the two states stay in
lockstep instead of one leg forking.
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import CASES, EVM_PY, HERE, load_json, replay_epoch
from differ import diff_case, print_report


def _run(cmd, tag) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    for ln in out.splitlines():
        if ln.startswith(("[evm]", "[avm]")):
            print("  " + ln)
    if p.returncode != 0:
        lines = [l for l in out.strip().splitlines() if l.strip()]
        print(f"  !! {tag} leg failed (rc={p.returncode}):")
        print("     " + "\n     ".join(lines[-6:]))
        # Carry the real cause into the batch summary (a bare exit code there is
        # useless when triaging a whole sweep in the morning).
        why = next((l for l in reversed(lines)
                    if any(k in l for k in ("Error", "error", "revert", "assert",
                                            "Exception", "failed"))), lines[-1] if lines else "")
        raise SystemExit(f"{tag} leg failed: {why.strip()[:160]}")
    return out


# Replaying a case advances LocalNet's clock by that case's whole historical
# span, and the clock is a RATCHET — it survives algod restarts and only a
# chain wipe rewinds it. So the shared base creeps into the future across a
# batch, and a far-future epoch silently costs coverage: DEGEN's constructor
# deploys at 2026 and fails at 2028, dropping the case from the corpus with a
# message that reads like an unrelated harness error.
#
# Set from measurement, not taste: DEGEN replays clean at +938 d and fails at
# roughly +1500 d. Warning at a month would fire on nearly every case after the
# first and train the reader to ignore it.
_DRIFT_WARN = 365 * 24 * 3600


def _chain_now() -> int:
    """Current LocalNet block time — the floor for the shared replay base,
    since the clock can only move forward from here."""
    try:
        sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))
        from framework.localnet import LocalNet
        a = LocalNet().algod
        now = int(a.block_info(a.status()["last-round"])["block"]["ts"])
    except Exception:
        return 0
    drift = now - int(time.time())
    if drift > _DRIFT_WARN:
        print(f"  ⚠️  LocalNet clock is {drift // 86400}d ahead of wall clock — "
              f"time-gated constructors start failing. Run `algokit localnet "
              f"reset` to rewind it (nothing in this suite persists there).")
    return now


def replay(tag: str, max_txns: int = 300, snapshot_every: int = 25,
           evm_layout: bool = False, evm_memory: bool = False,
           split_config: str | None = None,
           force_delegate: list[str] | None = None,
           child_box: bool = False) -> dict:
    case_dir = CASES / tag
    case = load_json(case_dir / "case.json")
    skips: dict[int, str] = {}
    # Each pass can UNCOVER new platform limits: skipping one txn changes the
    # group composition and opcode budget of the others, so 3 attempts was not
    # always enough. Leftovers are worse than slow — an unskipped platform-limit
    # txn runs on the EVM leg only, forking the state and reporting the
    # divergence as a storage difference (friend.tech: 10 stragglers ⇒ 20 bogus
    # sharesBalance entries).
    for attempt in range(1, 8):
        # Re-derived per attempt: the previous attempt's AVM run pushed the
        # chain clock forward, and both legs of THIS attempt must share a base
        # at or above it. Fixing it once outside the loop silently disabled
        # pinning on every re-run after a platform-limit skip.
        # Prefer TRUE historical time: a freshly reset LocalNet sits at ts=0,
        # so the window's own epoch is reachable and both legs replay at the
        # real historical instants. Only once the chain has ratcheted past it
        # does the base become "just ahead of the chain" and the epoch shift.
        epoch = replay_epoch(case.get("txns") or [])
        base = max(_chain_now() + 1, epoch)
        _run([str(EVM_PY), str(HERE / "evm_leg.py"), str(case_dir),
              json.dumps({"max_txns": max_txns, "snapshot_every": snapshot_every,
                          "pin_time": True, "time_base": base,
                          "skips": {str(k): v for k, v in skips.items()}})], "evm")
        evm_clock = load_json(case_dir / "evm_results.json")
        effective_base = int(evm_clock.get("time_base") or base)
        deployment_time = int(
            evm_clock.get("deployment_time") or effective_base)
        if attempt == 1:
            shift = effective_base - epoch
            label = ("historical" if shift == 0
                     else f"shifted +{shift // 86400}d")
            print(f"  [clock] {label} — shared monotonic schedule, "
                  f"base={effective_base}")
        _run(["python3", str(HERE / "avm_leg.py"), str(case_dir),
              json.dumps({"skips": [str(k) for k in skips],
                          "pin_time": True,
                          "time_base": effective_base,
                          "deployment_time": deployment_time,
                          "evm_layout": evm_layout,
                          "evm_memory": evm_memory,
                          "split_config": split_config,
                          "child_box": child_box,
                          "force_delegate": force_delegate or []})], "avm")
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
    split_config = opt("--split-config")
    force_delegate_raw = opt("--force-delegate")
    force_delegate = ([x.strip() for x in force_delegate_raw.split(",")
                       if x.strip()] if force_delegate_raw else [])
    force_fetch = "--fetch" in argv
    if force_fetch:
        argv.remove("--fetch")
    if "--evm-layout" in argv or "--evm-memory" in argv:
        sys.exit(
            "--evm-layout/--evm-memory are unavailable; use "
            "--evm-storage-layout for slot-compatible storage only")
    evm_layout = "--evm-storage-layout" in argv
    if evm_layout:
        argv.remove("--evm-storage-layout")
    evm_memory = False
    child_box = "--child-programs-via-box" in argv
    if child_box:
        argv.remove("--child-programs-via-box")
    if not argv:
        sys.exit(__doc__)
    tag = argv[0]

    if force_fetch or not (CASES / tag / "case.json").exists():
        if not (host and address):
            sys.exit(f"case {tag} not fetched — pass --host and --address")
        from fetch import fetch_case
        fetch_case(host, address, tag, max_txns)

    print(f"[replay] {tag}: max_txns={max_txns}"
          + (" [--evm-storage-layout]" if evm_layout else ""))
    print_report(replay(tag, max_txns, snap, evm_layout, evm_memory,
                        split_config, force_delegate, child_box))


if __name__ == "__main__":
    main()
