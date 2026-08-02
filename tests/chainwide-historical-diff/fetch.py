#!/usr/bin/env python3
"""Fetch a verified contract's source + ABI + constructor args + ASCENDING txn
history from a Blockscout instance (keyless) into cases/<tag>/.

  python3 fetch.py <host> <address> <tag> [--max-txns N]

Sources:
  /api/v2/smart-contracts/{addr}   verified source, ABI, compiler, ctor args
  /api?module=account&action=txlist&sort=asc   ascending history incl. creation
"""
from __future__ import annotations

import shutil
import sys
import time

from chd_common import CASES, EVM_PY, dump_json, http_json, relax_pragma


def _decode_ctor_addresses(abi, ctor_hex):
    """Address-typed constructor args (incl. address[] elements), decoded via
    the EVM venv's eth_abi (the system python doesn't carry it)."""
    import json as _json
    import subprocess
    ctor = next((e for e in abi if e.get("type") == "constructor"), None)
    if not ctor or not ctor.get("inputs") or not ctor_hex:
        return []
    script = (
        "import sys, json\n"
        "from eth_abi import decode\n"
        "types, hexdata = json.loads(sys.argv[1]), sys.argv[2]\n"
        "vals = decode(types, bytes.fromhex(hexdata))\n"
        "out = []\n"
        "def walk(v, t):\n"
        "    if t == 'address': out.append(v.lower())\n"
        "    elif t.endswith(']'):\n"
        "        base = t[:t.rindex('[')]\n"
        "        for x in v: walk(x, base)\n"
        "for v, t in zip(vals, types): walk(v, t)\n"
        "print(json.dumps(out))\n")
    def ctype(inp):
        t = inp["type"]
        if t.startswith("tuple"):
            return "(" + ",".join(ctype(c) for c in inp.get("components", [])) + ")" + t[len("tuple"):]
        return t
    types = [ctype(i) for i in ctor["inputs"]]
    try:
        p2 = subprocess.run([str(EVM_PY), "-c", script,
                             _json.dumps(types), ctor_hex],
                            capture_output=True, text=True, timeout=60)
        return _json.loads(p2.stdout.strip() or "[]")
    except Exception:
        return []


def fetch_dep(host: str, address: str, dep_dir, depth: int, seen: set) -> dict | None:
    """LIGHT dependency fetch: verified source + ABI + its own ctor args — no
    txn history (deps are only deployed, never replayed directly). Returns the
    dep's case dict (with nested "ctor_deps") or None when unusable.
    Single-file verifications only in v1."""
    addr = address.lower()
    if addr in seen or depth <= 0:
        return None
    seen.add(addr)
    try:
        sc = http_json(f"https://{host}/api/v2/smart-contracts/{address}")
    except Exception:
        return None
    if not sc.get("source_code"):
        return None
    comp = sc.get("compiler_version") or ""
    if "0.8." not in comp:
        return None
    if sc.get("additional_sources"):
        return None                      # v1: single-file deps only
    abi = sc.get("abi") or []
    ctor_hex = (sc.get("constructor_args") or "").removeprefix("0x")
    dep_dir.mkdir(parents=True, exist_ok=True)
    (dep_dir / "prepared.sol").write_text(relax_pragma(sc["source_code"]))
    dep = {"address": addr, "name": sc.get("name"),
           "compiler_version": comp, "abi": abi,
           "ctor_args_hex": ctor_hex, "ctor_deps": []}
    for sub in _decode_ctor_addresses(abi, ctor_hex):
        subdir = dep_dir / f"dep_{sub[2:10]}"
        d2 = fetch_dep(host, sub, subdir, depth - 1, seen)
        if d2:
            dep["ctor_deps"].append({"addr": sub, "dir": subdir.name})
    dump_json(dep_dir / "case.json", dep)
    return dep


def fetch_case(host: str, address: str, tag: str, max_txns: int = 300) -> dict:
    case_dir = CASES / tag
    addr = address.lower()

    # 1. verified source + metadata
    sc = http_json(f"https://{host}/api/v2/smart-contracts/{address}")
    if not sc.get("source_code"):
        sys.exit(f"[fetch] {tag}: contract not verified on {host}")
    comp = sc.get("compiler_version") or ""
    if "0.8." not in comp:
        sys.exit(f"[fetch] {tag}: compiler {comp} — v1 supports ^0.8.x only")
    abi = sc.get("abi") or []
    ctor_hex = (sc.get("constructor_args") or "").removeprefix("0x")

    # 2. ascending txn history via the Etherscan-compat API Blockscout hosts
    txns, page = [], 1
    creation = None
    while len(txns) < max_txns:
        url = (f"https://{host}/api?module=account&action=txlist&address={address}"
               f"&sort=asc&page={page}&offset=1000")
        d = http_json(url)
        rows = d.get("result") or []
        if not isinstance(rows, list) or not rows:
            break
        for t in rows:
            to = (t.get("to") or "").lower()
            frm = (t.get("from") or "").lower()
            if creation is None and not to:                 # creation txn
                creation = {"creator": frm,
                            "hash": t.get("hash"),
                            "ts": int(t.get("timeStamp") or 0),
                            "block": int(t.get("blockNumber") or 0)}
                continue
            if to != addr:
                continue
            txns.append({
                "hash": t.get("hash"),
                "from": frm,
                "input": t.get("input") or "0x",
                "value": int(t.get("value") or 0),
                "hist_ok": (t.get("txreceipt_status") == "1"
                            and t.get("isError") == "0"),
                "ts": int(t.get("timeStamp") or 0),
                "block": int(t.get("blockNumber") or 0),
            })
            if len(txns) >= max_txns:
                break
        if len(rows) < 1000:
            break
        page += 1
        time.sleep(0.4)                                    # be polite

    if creation is None:
        # creation may pre-date the window only if pagination missed it — for
        # sort=asc page 1 it's always first; fall back to v2 address info.
        try:
            ai = http_json(f"https://{host}/api/v2/addresses/{address}")
        except Exception:
            ai = {}
        creation = {"creator": (ai.get("creator_address_hash") or "").lower() or None,
                    "hash": ai.get("creation_tx_hash"), "ts": 0, "block": 0}

    case = {
        "tag": tag, "host": host, "address": addr,
        "name": sc.get("name"),
        "compiler_version": comp,
        "creation": creation,
        "ctor_args_hex": ctor_hex,
        "abi": abi,
        "txns": txns,
    }
    # Source layout. Single-file → prepared.sol. Multi-file (the majority of
    # modern verifications: ~86% of Base's popular ERC-20s) → materialise the
    # real file TREE plus the verification's remappings, which both legs can
    # consume natively (solc standard-json sources+remappings; puya-sol
    # --source per file + --import-path + --remapping).
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / "source.sol").write_text(sc["source_code"])
    (case_dir / "prepared.sol").write_text(relax_pragma(sc["source_code"]))
    extra = sc.get("additional_sources") or []
    if extra:
        main_rel = sc.get("file_path") or "Main.sol"
        def _rel(p):                     # may be absolute in the API payload
            return str(p).lstrip("/") or "Main.sol"
        main_rel = _rel(main_rel)
        tree = {main_rel: relax_pragma(sc["source_code"])}
        for f in extra:
            tree[_rel(f["file_path"])] = relax_pragma(f.get("source_code", ""))
        src_root = case_dir / "src"
        shutil.rmtree(src_root, ignore_errors=True)
        for rel, content in tree.items():
            p = src_root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        case["multifile"] = {
            "main": main_rel,
            "files": sorted(tree),
            "remappings": [r.lstrip(":") for r in
                           (sc.get("compiler_settings") or {}).get("remappings") or []],
        }
    # ── constructor DEPENDENCIES: verified contracts the ctor args point at.
    # Fetched light (source+abi+ctor args, no history) and deployed FIRST on
    # both legs, with the historical address remapped to the local instance —
    # the fix for the "ctor calls an external contract" closed-world skips.
    ctor_deps = []
    seen = {addr}
    # hardcoded address literals in the source join the candidate set (the
    # memecoin pattern: UniswapV2Factory/Router baked in, zero ctor args)
    import re as _re
    literals = {("0x" + m.lower()) for m in
                _re.findall(r"0x([0-9a-fA-F]{40})\b", sc["source_code"])}
    for a in list(_decode_ctor_addresses(abi, ctor_hex)) + sorted(literals):
        depdir = case_dir / "deps" / f"dep_{a[2:10]}"
        d = fetch_dep(host, a, depdir, depth=2, seen=seen)
        if d:
            ctor_deps.append({"addr": a, "dir": f"deps/dep_{a[2:10]}"})
            print(f"[fetch] {tag}: dep {d['name']} @ {a[:10]}… fetched")
    if ctor_deps:
        case["ctor_deps"] = ctor_deps
    dump_json(case_dir / "case.json", case)
    mf = case.get("multifile")
    print(f"[fetch] {tag}: {sc.get('name')} solc={comp[:12]} "
          f"{'MULTI-FILE('+str(len(mf['files']))+' files)' if mf else 'single-file'} "
          f"creator={(creation['creator'] or '?')[:10]}… txns={len(txns)} "
          f"ctor_hex={len(ctor_hex)//2}B → {case_dir}")
    return case


def main():
    argv = list(sys.argv[1:])
    max_txns = 300
    if "--max-txns" in argv:
        i = argv.index("--max-txns")
        max_txns = int(argv[i + 1]); del argv[i:i + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    fetch_case(argv[0], argv[1], argv[2], max_txns)


if __name__ == "__main__":
    main()
