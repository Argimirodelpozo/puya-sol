#!/usr/bin/env python3
"""Find COMPLEX, PURE-Solidity deployed contracts (no inline assembly).

Assembly is what forces `--evm-storage-layout` on us: a contract that reads
`.slot`/`sload` needs our storage to be EVM-shaped. Without any assembly the
default (native) storage model is free to differ, so these contracts exercise
the compiler's mainstream path — and they are the ones worth replaying.

Ranks candidates by a crude complexity score (source size, function count,
transaction count) and prints a fetch-ready table. HTTP only; no chain work.
"""
from __future__ import annotations

import json
import re
import sys
import time

from chd_common import http_json

UA = {"User-Agent": "Mozilla/5.0"}


def sources_of(host: str, addr: str):
    """Full source text + metadata, or None when unverified/unavailable."""
    try:
        d = http_json(f"https://{host}/api/v2/smart-contracts/{addr}", timeout=30)
    except Exception:
        return None
    if not d or not d.get("source_code"):
        return None
    parts = [d.get("source_code") or ""]
    for extra in (d.get("additional_sources") or []):
        parts.append(extra.get("source_code") or "")
    return {
        "name": d.get("name") or "?",
        "compiler": d.get("compiler_version") or "",
        "proxy": bool(d.get("is_proxy")),
        "files": 1 + len(d.get("additional_sources") or []),
        "text": "\n".join(parts),
    }


# `assembly {` but not inside a comment line
_ASM = re.compile(r'^[^/\n]*\bassembly\s*(\("[^"\n]*"\)\s*)?\{', re.M)
_FUNC = re.compile(r"^\s*function\s+\w+", re.M)
# Only STORAGE-touching assembly forces --evm-storage-layout on us. Memory /
# returndata assembly (mstore, returndatasize, calldatacopy) runs fine on the
# native model, and the corpus already replays plenty of it.
_ASM_STORAGE = re.compile(r"\.slot\b|\.offset\b|\bsload\b|\bsstore\b|\btload\b|\btstore\b")
# delegatecall is a HARD error on AVM (shared-storage / caller-preservation has
# no equivalent) — even when it sits in dead library code, so it disqualifies a
# candidate today just as surely as storage assembly does.
_DELEGATE = re.compile(r"\bdelegatecall\b")


def assess(host: str, addr: str):
    src = sources_of(host, addr)
    if not src:
        return None
    text = src["text"]
    asm = len(_ASM.findall(text))
    asm_storage = len(_ASM_STORAGE.findall(text))
    deleg = len(_DELEGATE.findall(text))
    funcs = len(_FUNC.findall(text))
    loc = text.count("\n")
    ver = src["compiler"]
    ok_ver = ver.startswith(("v0.8", "0.8"))
    try:
        cnt = http_json(f"https://{host}/api/v2/addresses/{addr}/counters", timeout=25)
        txns = int((cnt or {}).get("transactions_count") or 0)
    except Exception:
        txns = 0
    return {
        "addr": addr, "name": src["name"], "compiler": ver[:12],
        "proxy": src["proxy"], "files": src["files"],
        "asm": asm, "asm_storage": asm_storage, "funcs": funcs,
        "loc": loc, "txns": txns,
        # eligible = no STORAGE assembly (memory assembly is fine)
        "deleg": deleg,
        "eligible": (asm_storage == 0 and deleg == 0 and ok_ver
                     and not src["proxy"] and txns >= 30),
        # complexity: functions dominate, size and traffic contribute
        "score": funcs * 10 + loc // 50 + min(txns, 5000) // 100,
    }


def main(argv):
    host = argv[0] if argv else "eth.blockscout.com"
    addrs = argv[1:]
    if not addrs:
        print("usage: scan_pure.py <host> <addr> [addr...]")
        return 1
    rows = []
    for a in addrs:
        r = assess(host, a)
        if r:
            rows.append(r)
            flag = "PURE" if r["eligible"] else "----"
            print(f"  {flag} {r['name'][:26]:26s} asm={r['asm']:3d}/st{r['asm_storage']:<3d} dc={r['deleg']:<3d} fns={r['funcs']:3d} "
                  f"loc={r['loc']:5d} txns={r['txns']:6d} {r['compiler']:12s} "
                  f"{'PROXY ' if r['proxy'] else ''}{a[:12]}", flush=True)
        else:
            print(f"  ???? unverified/unavailable {a[:12]}", flush=True)
        time.sleep(0.3)
    good = sorted((r for r in rows if r["eligible"]),
                  key=lambda r: -r["score"])
    print(f"\n=== {len(good)} PURE-Solidity candidates, most complex first ===")
    for r in good:
        print(f"  {r['score']:5d}  {r['name'][:28]:28s} fns={r['funcs']:3d} "
              f"loc={r['loc']:5d} txns={r['txns']:6d}  {r['addr']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
