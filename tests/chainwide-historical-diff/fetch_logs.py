#!/usr/bin/env python3
"""Fetch historical receipt EVENT LOGS for a case's transactions.

  python3 fetch_logs.py <host> <tag> [<tag> ...]

Reads cases/<tag>/calls.json, pulls each root transaction's receipt logs from
Blockscout (`/api/v2/transactions/{hash}/logs`, keyless, paced), and writes
cases/<tag>/logs.json as {hash: [{address, topics, data}, ...]}.

The historical receipt is the EVENT ORACLE for the joint CCTP replay: unlike
the per-contract lane (which diffs local-EVM vs AVM events), the chain's own
logs are ground truth here, so no second leg is needed for event comparison.

Lifted internal calls ('#' in the hash) share their parent's receipt; only
unique parent hashes are fetched. Already-fetched hashes are kept (resumable).
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

from chd_common import http_json

CASES = Path(__file__).parent / "cases"


def fetch_tx_logs(host: str, tx_hash: str) -> list[dict]:
    """All receipt logs for one transaction, following Blockscout paging."""
    out: list[dict] = []
    params = ""
    while True:
        page = None
        for attempt in range(4):
            try:
                page = http_json(
                    f"https://{host}/api/v2/transactions/{tx_hash}/logs{params}",
                    timeout=30,
                )
                break
            except Exception:
                time.sleep(1.0 * (2**attempt))
        if not isinstance(page, dict):
            raise RuntimeError(f"logs fetch failed for {tx_hash}")
        for item in page.get("items", []):
            out.append(
                {
                    "address": (item.get("address") or {}).get("hash", "").lower(),
                    "topics": [t for t in (item.get("topics") or []) if t],
                    "data": item.get("data") or "0x",
                    "index": item.get("index"),
                }
            )
        nxt = page.get("next_page_params")
        if not nxt:
            break
        params = "?" + "&".join(f"{k}={v}" for k, v in nxt.items())
    out.sort(key=lambda l: (l["index"] if l["index"] is not None else 1 << 30))
    return out


def main(argv: list[str]) -> int:
    cases_root = CASES
    if "--cases" in argv:
        i = argv.index("--cases")
        cases_root = Path(argv[i + 1])
        del argv[i : i + 2]
    if len(argv) < 2:
        print(__doc__)
        return 2
    host, tags = argv[0], argv[1:]
    for tag in tags:
        case_dir = cases_root / tag
        calls = json.loads((case_dir / "calls.json").read_text())
        hashes = []
        seen = set()
        for call in calls["calls"]:
            h = call["hash"].split("#")[0]
            if h not in seen:
                seen.add(h)
                hashes.append(h)
        out_path = case_dir / "logs.json"
        logs = json.loads(out_path.read_text()) if out_path.exists() else {}
        fetched = 0
        for i, h in enumerate(hashes):
            if h in logs:
                continue
            logs[h] = fetch_tx_logs(host, h)
            fetched += 1
            time.sleep(0.6)  # be polite
            if fetched % 25 == 0:
                out_path.write_text(json.dumps(logs))
                print(f"[logs] {tag}: {i + 1}/{len(hashes)} hashes", flush=True)
        out_path.write_text(json.dumps(logs))
        total = sum(len(v) for v in logs.values())
        print(f"[logs] {tag}: {len(logs)} txns, {total} log entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
