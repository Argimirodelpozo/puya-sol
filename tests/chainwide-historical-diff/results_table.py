#!/usr/bin/env python3
"""Regenerate the README's Results tables from the case reports on disk.

  python3 results_table.py            # markdown to stdout

Hand-maintained result tables go stale silently, and a stale number here reads
exactly like a verified one. Each cases/<tag>/report.json is that case's LAST
run, so re-run a case (in the mode you want to quote) before regenerating.
"""
from __future__ import annotations

import glob
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CASES = HERE / "cases"

_DIV_KEYS = ("status_div", "value_div", "event_div", "snapshot_div",
             "storage_div", "storage_map_div")


def candidate_notes() -> dict:
    """tag -> (host, address, trailing # comment) from batch.py's CANDIDATES."""
    src = (HERE / "batch.py").read_text()
    m = re.search(r"CANDIDATES\s*=\s*\[(.*?)\n\]", src, re.S)
    out = {}
    for line in m.group(1).splitlines() if m else []:
        mm = re.match(r'\s*\("([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\),\s*(?:#\s*(.*))?',
                      line)
        if mm:
            out[mm.group(3)] = (mm.group(1), mm.group(2), (mm.group(4) or "").strip())
    return out


def chain_of(host: str) -> str:
    return (host or "?").split(".")[0].replace("eth", "ethereum")


def load_rows():
    notes = candidate_notes()
    rows = []
    for p in sorted(glob.glob(str(CASES / "*" / "report.json"))):
        tag = Path(p).parent.name
        try:
            r = json.loads(Path(p).read_text())
        except Exception:
            continue
        c = r.get("counts") or {}
        host, _addr, note = notes.get(tag, ("", "", ""))
        if not host:                       # ad-hoc case, not in CANDIDATES
            try:
                host = json.loads((CASES / tag / "case.json").read_text()).get("host", "?")
            except Exception:
                host = "?"
        rows.append({
            "tag": tag,
            "name": r.get("name") or "?",
            "chain": chain_of(host),
            "replayed": r.get("replayed") or 0,
            "window": r.get("txns_in_window") or 0,
            "div": sum(c.get(k, 0) for k in _DIV_KEYS),
            "skips": r.get("skips") or {},
            "note": note,
        })
    return rows


def main():
    rows = load_rows()
    clean = [r for r in rows if r["div"] == 0]
    dirty = [r for r in rows if r["div"] > 0]
    tot_rep = sum(r["replayed"] for r in rows)
    tot_win = sum(r["window"] for r in rows)
    skips: dict[str, int] = {}
    for r in rows:
        for k, v in r["skips"].items():
            skips[k] = skips.get(k, 0) + v

    print("| | |")
    print("|---|---|")
    print(f"| contracts replayed | **{len(rows)}** |")
    print(f"| zero divergences | **{len(clean)}** |")
    print(f"| with divergences | {len(dirty)} |")
    print(f"| transactions replayed on both legs | **{tot_rep:,}** of {tot_win:,} "
          f"in-window ({100 * tot_rep / max(tot_win, 1):.0f}%) |")
    print("| skipped, by cause | "
          + ", ".join(f"{k} {v:,}" for k, v in sorted(skips.items(), key=lambda kv: -kv[1]))
          + " |")
    print()
    print("| contract | chain | replayed | divergences | skips |")
    print("|---|---|---|---|---|")
    for r in sorted(rows, key=lambda r: (-r["replayed"], r["tag"])):
        sk = ", ".join(f"{k} {v}" for k, v in sorted(r["skips"].items(),
                                                     key=lambda kv: -kv[1])) or "—"
        mark = "✅" if r["div"] == 0 else f"❌ {r['div']}"
        print(f"| `{r['tag']}` {r['name']} | {r['chain']} | "
              f"{r['replayed']}/{r['window']} | {mark} | {sk} |")


if __name__ == "__main__":
    sys.exit(main())
