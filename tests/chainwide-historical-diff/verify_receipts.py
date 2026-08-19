#!/usr/bin/env python3
"""Verify "history says ok, replay says rejected" rows against the CHAIN's own
execution trace, and emit auditable corpus corrections.

  python3 verify_receipts.py cctp_joint_report.json [--out receipt_corrections.json]

Why this exists: a Blockscout transaction's `status` field is not always the
execution outcome. In the 3000-txn CCTP window, 13 transactions carry
`status: ok` while their raw trace carries `error: Reverted`, they emit zero
logs, and their calldata's selector is not even present in the contract's
deployed bytecode (so a revert is the only possible outcome). The replay was
right and the corpus was wrong in all 13.

Hand-listing such hashes does not scale as windows deepen, and "trust the
replay" would be circular. This asks the chain instead:

  candidate  = replay rejected it, history claims success
  evidence   = the transaction's TOP-LEVEL trace entry reports an error
  correction = historical_ok -> False, recorded with the evidence that proved it

Only that direction is ever emitted (claimed-success -> failed). A row whose
trace shows real execution is left alone and stays a genuine mismatch, which
is the whole point: this must not be able to launder a compiler bug into a
corpus correction.

Output merges into receipt_corrections.json — tracked in git, because it
is audited evidence rather than corpus bulk — which oracle_cctp_historical.py
loads into MAINNET_RECEIPT_METADATA on startup.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import http_json

HOST = "eth.blockscout.com"


def top_level_error(host: str, tx_hash: str) -> str | None:
    """The error on the transaction's OUTERMOST call, or None if it ran."""
    trace = http_json(f"https://{host}/api/v2/transactions/{tx_hash}/raw-trace")
    if not isinstance(trace, list) or not trace:
        return None
    # The first entry is the root call; nested entries are its children, whose
    # failures do NOT imply the transaction failed (CCTP swallows some).
    return trace[0].get("error")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("report", type=Path)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--cases", type=Path,
                    default=Path(__file__).parent / "cases")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--limit", type=int, default=200)
    args = ap.parse_args()
    out_path = args.out or (Path(__file__).parent / "receipt_corrections.json")

    rep = json.loads(args.report.read_text())
    candidates = [
        r for r in rep["results"]
        if not r.get("matched_status")
        and r.get("historical_ok")
        and r.get("oracle_result") != "ACCEPT"
    ]
    print(f"{len(candidates)} candidate row(s): history claims success, "
          f"replay rejected")

    existing = {}
    if out_path.exists():
        existing = json.loads(out_path.read_text())

    verified, ran_fine, failed_lookup = 0, [], 0
    for r in candidates[: args.limit]:
        h = r["hash"].split("#")[0]
        if h in existing:
            continue
        try:
            err = top_level_error(args.host, h)
        except Exception as e:
            failed_lookup += 1
            print(f"  ? {h[:20]} trace lookup failed ({str(e)[:50]})")
            continue
        if err:
            existing[h] = {
                "historical_ok": False,
                "evidence": {
                    "source": "blockscout raw-trace, top-level call",
                    "error": err,
                    "signature": r.get("signature"),
                    "block": r.get("block"),
                },
            }
            verified += 1
            print(f"  ✓ {h[:20]} top-level call: {err} → corrected to failed")
        else:
            ran_fine.append(h)
            print(f"  ! {h[:20]} trace shows NO top-level error — genuine "
                  f"mismatch, left alone")
        time.sleep(0.4)

    out_path.write_text(json.dumps(existing, indent=1))
    print(f"\n{verified} correction(s) added, {len(ran_fine)} left as real "
          f"mismatches, {failed_lookup} lookup failure(s)")
    print(f"wrote {out_path} ({len(existing)} total)")
    if ran_fine:
        print("\nGENUINE MISMATCHES (the chain executed these):")
        for h in ran_fine:
            print("   ", h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
