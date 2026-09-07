#!/usr/bin/env python3
"""Replay a chosen list of already-fetched cases on the per-contract LocalNet
lane and compare each fresh report against the stored one.

  python3 run_subset.py <out-dir> <tag>[,<tag>...] [--slot <tag>[,<tag>...]]

For every tag, the existing cases/<tag>/report.json and avm_results.json are
snapshotted into <out-dir>/prev/ (first time only, so repeated runs keep the
ORIGINAL baseline), the case is replayed with replay.replay() at the window its
stored report used (200 when there is none; --slot tags run in
--evm-storage-layout mode), and one summary line compares replayed count, real
divergence buckets and storage coverage (typed_boxes / unattributed /
holder_roots / holder_mismatch) against the baseline.  A running
<out-dir>/summary.json accumulates every tag's before/after so a batch can be
interrupted and resumed.

Requires LocalNet and build/puya-sol, exactly like replay.py.  NOTE: the
per-contract lane compiles cases/<tag>/out_avm with --contract-abi evm; for
the CCTP v1 cases that overwrites the joint oracle lane's ARC-4 artifacts —
run refresh_cctp_artifacts.py afterwards (oracle_cctp_historical.py refuses
the clobbered artifacts and says so).
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from chd_common import CASES, dump_json, load_json  # noqa: E402


def real_count(counts: dict) -> int:
    from differ import _REAL_BUCKETS

    return sum(int(counts.get(k, 0)) for k in _REAL_BUCKETS)


def coverage_of(avm_results: Path) -> dict:
    try:
        return (load_json(avm_results).get("storage") or {}).get("coverage") or {}
    except Exception:
        return {}


def run_one(tag: str, out: Path, slot_mode: bool, summary: list[dict]) -> dict:
    from differ import print_report
    from replay import replay

    case_dir = CASES / tag
    prev_dir = out / "prev"
    prev_dir.mkdir(parents=True, exist_ok=True)
    report_path = case_dir / "report.json"
    prev_report = load_json(report_path) if report_path.exists() else {}
    if prev_report and not (prev_dir / f"{tag}.report.json").exists():
        shutil.copy(report_path, prev_dir / f"{tag}.report.json")
        if (case_dir / "avm_results.json").exists():
            shutil.copy(case_dir / "avm_results.json", prev_dir / f"{tag}.avm_results.json")
    prev_cov = coverage_of(prev_dir / f"{tag}.avm_results.json")
    window = int(prev_report.get("txns_in_window") or 200)
    entry = {
        "tag": tag,
        "window": window,
        "slot_mode": slot_mode,
        "prev": {
            "replayed": prev_report.get("replayed"),
            "real": real_count(prev_report.get("counts") or {}),
            "counts": prev_report.get("counts"),
            "coverage": prev_cov,
        },
    }
    print(f"\n{'=' * 70}\n[subset] {tag} window={window} slot={slot_mode}\n{'=' * 70}", flush=True)
    started = time.time()
    try:
        rep = replay(tag, window, evm_layout=slot_mode)
        print_report(rep)
        counts = rep["counts"]
        entry.update(
            {
                "status": "done",
                "name": rep.get("name"),
                "replayed": rep["replayed"],
                "skips": rep["skips"],
                "real": real_count(counts),
                "counts": counts,
                "coverage": coverage_of(case_dir / "avm_results.json"),
                "elapsed": round(time.time() - started),
            }
        )
    except SystemExit as error:
        entry.update({"status": "skipped", "why": str(error)[:300], "elapsed": round(time.time() - started)})
        print(f"[subset] {tag}: SKIPPED — {str(error)[:300]}", flush=True)
    except Exception as error:  # noqa: BLE001 — one bad case must not end the batch
        entry.update(
            {
                "status": "error",
                "why": f"{type(error).__name__}: {error}"[:300],
                "elapsed": round(time.time() - started),
            }
        )
        print(f"[subset] {tag}: ERROR — {type(error).__name__}: {error}", flush=True)
        traceback.print_exc()
    summary[:] = [s for s in summary if s["tag"] != tag] + [entry]
    dump_json(out / "summary.json", summary)
    flag = {"done": "OK" if not entry.get("real") else "DIV"}.get(entry["status"], "--")
    cov = entry.get("coverage") or {}
    print(
        f"[subset] {flag} {tag:<22} replayed={entry.get('replayed')}/{window} "
        f"(prev {entry['prev']['replayed']}) real={entry.get('real')} "
        f"(prev {entry['prev']['real']}) typed_boxes={cov.get('typed_boxes')} "
        f"(prev {prev_cov.get('typed_boxes')}) unattributed={cov.get('unattributed')} "
        f"(prev {prev_cov.get('unattributed')}) holder_roots={cov.get('holder_roots')} "
        f"mismatch={len(cov.get('holder_mismatch') or [])} {entry.get('elapsed')}s",
        flush=True,
    )
    return entry


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", type=Path, help="output dir (prev/ snapshots + summary.json)")
    parser.add_argument("tags", help="comma-separated case tags to replay")
    parser.add_argument("--slot", default="", help="comma-separated tags to run in --evm-storage-layout mode")
    args = parser.parse_args(argv)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    tags = [t for t in args.tags.split(",") if t]
    slot_tags = {t for t in args.slot.split(",") if t}
    summary_path = out / "summary.json"
    summary = load_json(summary_path) if summary_path.exists() else []
    for tag in tags:
        run_one(tag, out, tag in slot_tags, summary)
    print("[subset] DONE", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
