#!/usr/bin/env python3
"""Survey what actually blocks real deployed contracts from compiling.

  python3 survey.py <host> [--tokens ERC-20|ERC-721] [--pages N] [--limit N]

Samples verified contracts from a Blockscout listing, compiles each with
puya-sol (frontend only — fast, no backend), and buckets the outcome. The point
is a RANKED BLOCKER HISTOGRAM: which unsupported constructs gate the largest
share of real-world code, so compiler effort can be aimed at the ones that
actually unlock contracts.

Counts each contract ONCE, by its first (primary) error.
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from urllib.parse import quote

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import REPO, dump_json, http_json, relax_pragma

PUYA_SOL = REPO / "build" / "puya-sol"

# error text → short bucket. Order matters (first match wins).
BUCKETS = [
    (r"address\(addr\)\.code",              "address.code (ERC-721 receiver hook)"),
    (r"try/catch",                          "try/catch"),
    (r"tx\.origin",                         "tx.origin"),
    (r"delegatecall",                       "delegatecall"),
    (r"coerce non-scalar.*assembly",        "asm memory model (aggregate as asm ptr)"),
    (r"assembly",                           "inline assembly (other)"),
    (r"blockhash",                          "blockhash"),
    (r"selfdestruct",                       "selfdestruct"),
    (r"exceeds AVM 8KB|extra_pages",        "8KB program cap"),
    (r"assignment target type differs",     "type mismatch (POSSIBLE BUG)"),
    (r"unsupported type cast|cannot coerce", "type coercion gap (POSSIBLE BUG)"),
]


def classify(out: str) -> str:
    errs = [l for l in out.splitlines() if "error:" in l]
    if not errs:
        return "OK"
    first = errs[0]
    for pat, name in BUCKETS:
        if re.search(pat, first, re.I):
            return name
    m = re.search(r"error:\s*(.*)", first)
    return "other: " + (m.group(1)[:60] if m else first[:60])


def main():
    argv = sys.argv[1:]
    host = argv[0] if argv and not argv[0].startswith("--") else "eth.blockscout.com"

    def opt(flag, d, cast=int):
        return cast(argv[argv.index(flag) + 1]) if flag in argv else d

    tok = opt("--tokens", None, str)
    pages, limit = opt("--pages", 3), opt("--limit", 100)

    seen, results, params = set(), [], ""
    for _ in range(pages):
        base = (f"https://{host}/api/v2/tokens?type={tok}" if tok
                else f"https://{host}/api/v2/smart-contracts?filter=solidity")
        try:
            d = http_json(base + params, timeout=35)
        except Exception as e:
            print(f"[survey] listing failed: {str(e)[:70]}", file=sys.stderr)
            break
        for it in d.get("items") or []:
            if len(results) >= limit:
                break
            addr = it.get("address_hash") or (it.get("address") or {}).get("hash")
            if not addr or addr in seen:
                continue
            seen.add(addr)
            try:
                sc = http_json(f"https://{host}/api/v2/smart-contracts/{addr}", timeout=30)
            except Exception:
                continue
            if not sc.get("source_code"):
                continue
            comp = sc.get("compiler_version") or ""
            name = str(sc.get("name") or "?")
            if sc.get("additional_sources"):
                results.append((addr, name, comp, "multi-file source (harness v1 limit)"))
                continue
            if "0.8." not in comp:
                results.append((addr, name, comp, "pre-0.8 solc"))
                continue
            with tempfile.TemporaryDirectory() as td:
                p = Path(td) / "c.sol"
                p.write_text(relax_pragma(sc["source_code"]))
                try:
                    r = subprocess.run(
                        ["env", "-u", "PYTHONPATH", str(PUYA_SOL), "--source", str(p),
                         "--output-dir", td, "--puya-path", "/bin/true"],
                        capture_output=True, text=True, timeout=120)
                    verdict = classify(r.stdout + r.stderr)
                except subprocess.TimeoutExpired:
                    verdict = "compile timeout"
            results.append((addr, name, comp, verdict))
            print(f"  {name[:26]:<26} {verdict}", flush=True)
        npp = d.get("next_page_params")
        if not npp or len(results) >= limit:
            break
        params = "&" + "&".join(f"{k}={quote(str(v), safe='')}"
                                for k, v in npp.items() if v is not None)

    hist = Counter(v for _, _, _, v in results)
    n = len(results)
    print(f"\n{'='*74}\n[survey] {host} {tok or 'verified'} — {n} contracts\n{'='*74}")
    for k, c in hist.most_common():
        bar = "█" * max(1, round(40 * c / n)) if n else ""
        print(f"  {c:>3} ({100*c/n:4.1f}%) {bar} {k}")
    ok = hist.get("OK", 0)
    print(f"\n  COMPILES: {ok}/{n} ({100*ok/n if n else 0:.1f}%)")
    dump_json(Path(__file__).resolve().parent / "cases" / f"_survey_{host.split('.')[0]}_{tok or 'all'}.json",
              {"host": host, "type": tok, "n": n,
               "histogram": dict(hist), "results": results})


if __name__ == "__main__":
    main()
