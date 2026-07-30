#!/usr/bin/env python3
"""Discover replayable candidates from a Blockscout verified-contracts listing.

  python3 harvest.py <host> [--pages N] [--min-txns N] [--want-dep]

Filters for what this harness can actually replay:
  · solidity, ^0.8.x, single-file verification
  · enough transaction history to be worth replaying
  · (--want-dep) a constructor that takes an `address` — i.e. a contract whose
    external dependency is INJECTED rather than hardcoded, which is the class
    the dependency-mock path can handle.

Prints ready-to-paste CANDIDATES tuples.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import http_json


def ctype(inp):
    t = inp["type"]
    if t.startswith("tuple"):
        return "(" + ",".join(ctype(c) for c in inp.get("components", [])) + ")" + t[len("tuple"):]
    return t


def main():
    argv = sys.argv[1:]
    host = argv[0] if argv and not argv[0].startswith("--") else "eth.blockscout.com"

    def opt(flag, d, cast=int):
        return cast(argv[argv.index(flag) + 1]) if flag in argv else d

    pages = opt("--pages", 3)
    min_txns = opt("--min-txns", 200)
    want_dep = "--want-dep" in argv

    # Source: recently-verified listing (fresh, little history) OR the token
    # listing (high-activity contracts — far better hit rate for replay).
    tok_type = None
    if "--tokens" in argv:
        tok_type = argv[argv.index("--tokens") + 1]        # e.g. ERC-20 / ERC-721

    seen, hits, params = set(), [], ""
    for page in range(pages):
        base = (f"https://{host}/api/v2/tokens?type={tok_type}" if tok_type
                else f"https://{host}/api/v2/smart-contracts?filter=solidity")
        try:
            d = http_json(base + params, timeout=35)
        except Exception as e:
            print(f"[harvest] listing page {page} failed: {str(e)[:80]}", file=sys.stderr)
            break
        items = d.get("items") or []
        for it in items:
            if tok_type:
                addr, name, comp = it.get("address_hash"), it.get("name") or "?", ""
            else:
                addr = (it.get("address") or {}).get("hash")
                name = (it.get("address") or {}).get("name") or "?"
                comp = it.get("compiler_version") or ""
            if not addr or addr in seen:
                continue
            seen.add(addr)
            # NOTE: the listing's transactions_count is null in practice, so
            # history is counted per-candidate below (after the cheap filters).
            # The token listing carries no compiler version — it comes from the
            # per-contract detail fetch instead.
            if not tok_type and "0.8." not in comp:
                continue
            if any(k in name for k in ("Proxy", "Beacon", "Implementation")):
                continue                                  # proxies: delegatecall, out of scope
            try:
                sc = http_json(f"https://{host}/api/v2/smart-contracts/{addr}", timeout=30)
            except Exception:
                continue
            if not sc.get("source_code"):
                continue
            nfiles = 1 + len(sc.get("additional_sources") or [])
            if nfiles > 1 and "--multifile" not in argv:
                continue          # multi-file now supported; opt in explicitly
            comp = sc.get("compiler_version") or comp
            if "0.8." not in comp:
                continue
            # Proxies are delegatecall-based => out of scope. In token mode the
            # listing name is the TOKEN name, so the real contract name is only
            # known here, after the detail fetch.
            cname = str(sc.get("name") or "")
            if any(k in cname for k in ("Proxy", "Beacon", "Upgradeable")):
                continue
            abi = sc.get("abi") or []
            ctor = next((e for e in abi if e.get("type") == "constructor"), None)
            dep_args = [i["name"] or "?" for i in (ctor or {}).get("inputs", [])
                        if ctype(i) == "address"]
            if want_dep and not dep_args:
                continue
            # does it call OUT? (interface-typed calls are the interesting bit)
            src = sc["source_code"]
            # Source-level pre-filter for constructs puya-sol deliberately
            # hard-errors on. Cheaper than compiling, and these are systematic:
            #   .code / try-catch  → ERC-721 receiver hook (blocks ~all NFTs)
            #   inline assembly    → aggregate-as-asm-pointer memory wall
            #   tx.origin          → unsupported by design
            if "--compilable" in argv:
                blockers = [k for k in (".code.length", "address(this).code",
                                        "try ", "tx.origin", "assembly {",
                                        "delegatecall")
                            if k in src and k != "address(this).code"]
                if blockers:
                    continue
            calls_out = any(k in src for k in ("IERC20(", "IERC721(", ".transferFrom(",
                                               ".safeTransfer", "interface I"))
            # only now (structural filters passed) pay for a history lookup
            try:
                tl = http_json(f"https://{host}/api?module=account&action=txlist"
                               f"&address={addr}&sort=asc&page=1&offset={min_txns + 5}",
                               timeout=30)
                n = len(tl.get("result") or []) if isinstance(tl.get("result"), list) else 0
            except Exception:
                n = 0
            if n < min_txns:
                continue
            hits.append((addr, sc.get("name"), comp, n, dep_args, nfiles))
            print(f"  {addr} {str(sc.get('name'))[:22]:<22} solc={comp[:10]} "
                  f"txns={n:<7} files={nfiles} ctor_addr={dep_args}", flush=True)
        npp = d.get("next_page_params")
        if not npp:
            break
        # names can contain spaces/unicode → must be percent-encoded
        from urllib.parse import quote
        params = "&" + "&".join(f"{k}={quote(str(v), safe='')}"
                                for k, v in npp.items() if v is not None)
        time.sleep(0.4)

    print(f"\n[harvest] {len(hits)} candidate(s) from {len(seen)} listed\n")
    for addr, name, comp, n, dep, nf in sorted(hits, key=lambda h: -h[3]):
        print(f'    ("{host}", "{addr}", "{str(name).lower()[:14]}"),'
              f'   # {n} txns, {nf} files, ctor_addr={dep}')


if __name__ == "__main__":
    main()
