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

from chd_common import (CASES, EVM_PY, ZERO, dump_json, http_json,
                        relax_pragma)


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


def harvest_callees(host: str, address: str, hashes: list, sink: dict,
                    max_traces: int = 12) -> None:
    """Sample DIRECT txns' traces for the contract's outgoing calls.

    Internal-call parents only cover router-driven traffic; most windows are
    dominated by direct txns, and their callees are what the closed-world
    filter trips over. Sampled, not exhaustive — one rate-limited request per
    trace, and a handful is enough to surface a contract's fixed collaborators
    (a factory, a token, an oracle).
    """
    addr = address.lower()
    for h in hashes[:max_traces]:
        try:
            tr = http_json(f"https://{host}/api/v2/transactions/{h}/raw-trace")
        except Exception:
            continue
        if not isinstance(tr, list):
            continue
        for e in tr:
            if not isinstance(e, dict) or e.get("type") != "call":
                continue
            act = e.get("action") or {}
            if (act.get("from") or "").lower() != addr:
                continue
            tgt = (act.get("to") or "").lower()
            if tgt and tgt != addr and tgt != ZERO:
                sink[tgt] = sink.get(tgt, 0) + 1
        time.sleep(0.6)


def fetch_internal_calls(host: str, address: str, block_lo: int, block_hi: int,
                         direct_hashes: set, max_parents: int = 200,
                         max_calls: int = 400, callee_sink: dict | None = None) -> list:
    """Calls INTO `address` made by other CONTRACTS (internal transactions).

    The address-level internal-txn APIs strip calldata (`input: "0x"` on the
    Etherscan-compat endpoint, absent entirely from v2), which is why this
    class of traffic was long considered unreplayable. The PER-TRANSACTION
    raw trace does carry it: Parity-format entries of
    `{action:{from,to,input,value,callType}, result, traceAddress, type}`.
    So: list the internal txns to get their PARENT hashes, then pull each
    parent's trace once and lift every non-root call whose `to` is us.

    Root entries (empty traceAddress) are skipped — those are the direct
    txns already in `txlist`.
    """
    addr = address.lower()
    # PARENT DISCOVERY via the TOKEN-TRANSFER index, not `txlistinternal`.
    # Explorers index "internal transactions" as VALUE-MOVING traces only —
    # 496/496 of PEPE's are plain ETH transfers with no calldata — so that
    # endpoint cannot find contract-to-contract CALLS. Every internal
    # `transfer`/`transferFrom` does emit a Transfer event though, and those
    # ARE indexed with their parent tx: take the parents in our block window
    # that aren't already direct txns, and read their traces.
    parents, seen_p = [], set()
    page = 1
    while len(parents) < max_parents:
        try:
            d = http_json(f"https://{host}/api?module=account&action=tokentx"
                          f"&contractaddress={address}"
                          f"&startblock={block_lo}&endblock={block_hi}"
                          f"&sort=asc&page={page}&offset=1000")
        except Exception:
            break
        rows = d.get("result") or []
        if not isinstance(rows, list) or not rows:
            break
        for r in rows:
            h = r.get("hash")
            if not h or h in seen_p or h.lower() in direct_hashes:
                continue
            seen_p.add(h)
            parents.append((h, int(r.get("blockNumber") or 0),
                            int(r.get("transactionIndex") or 0),
                            int(r.get("timeStamp") or 0)))
            if len(parents) >= max_parents:
                break
        if len(rows) < 1000:
            break
        page += 1
        time.sleep(0.4)
    parents.sort(key=lambda p: (p[1], p[2]))

    out = []
    dropped = 0
    for h, blk, txi, ts in parents:
        if len(out) >= max_calls:
            break
        # Public Blockscout rate-limits hard: a burst of raw-trace requests
        # comes back as 500s. Pace + retry with backoff, and COUNT what we
        # still lose — a silently-partial internal-call set would overstate
        # replay coverage.
        tr = None
        for attempt in range(3):
            try:
                tr = http_json(f"https://{host}/api/v2/transactions/{h}/raw-trace")
                break
            except Exception:
                time.sleep(1.0 * (2 ** attempt))
        if tr is None:
            dropped += 1
            continue
        if not isinstance(tr, list):
            continue
        for i, e in enumerate(tr):
            if not isinstance(e, dict) or e.get("type") != "call":
                continue
            act = e.get("action") or {}
            # Calls OUT of the contract, harvested from traces we already
            # fetched. These are the external contracts it genuinely depends
            # on — the ones whose absence makes a txn revert locally and get
            # dropped by the closed-world filter. Far better targeted than
            # "every address that appears in an argument", which is mostly
            # transfer recipients.
            if callee_sink is not None and (act.get("from") or "").lower() == addr:
                tgt = (act.get("to") or "").lower()
                if tgt and tgt != addr and tgt != ZERO:
                    callee_sink[tgt] = callee_sink.get(tgt, 0) + 1
            if not e.get("traceAddress"):
                continue                  # root call == the direct txn
            if (act.get("to") or "").lower() != addr:
                continue
            if (act.get("callType") or "call") != "call":
                continue                  # staticcall = view; delegatecall N/A
            inp = act.get("input") or "0x"
            if len(inp) < 10:
                continue                  # plain value transfer, no selector
            val = act.get("value") or "0x0"
            try:
                val = int(val, 16) if isinstance(val, str) else int(val)
            except Exception:
                val = 0
            res = e.get("result")
            out.append({
                "hash": f"{h}#{'.'.join(str(x) for x in e['traceAddress'])}",
                "from": (act.get("from") or "").lower(),
                "input": inp,
                "value": val,
                "hist_ok": bool(res) and not e.get("error"),
                "ts": ts, "block": blk,
                "internal": True,
                "trace_pos": i, "txindex": txi,
            })
            if len(out) >= max_calls:
                break
        time.sleep(0.6)
    if dropped:
        print(f"[fetch] internal calls: {dropped}/{len(parents)} parent trace(s) "
              f"unavailable after retries — those calls are NOT in the replay")
    return out


# A constructor that calls a dependency we cannot compile (a proxy, or pre-0.8
# like Polymarket's ConditionalTokens / USDC) fails to deploy on BOTH legs, so
# the case is unreachable — the single biggest cause of "not replayable
# standalone". For a dependency that is only used through a standard token
# interface, a minimal ^0.8 stand-in deployed IDENTICALLY on both legs restores
# the case: the differ compares puya-sol against solc, and both see the same
# stand-in, so its verdict stays sound. Fidelity to the real chain is NOT
# claimed — any txn whose result then disagrees with history is dropped by the
# closed-world filter exactly as before.
_STUB_ERC20 = """// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract StubERC20 {
    // name/symbol as PURE functions, not initialised state: an aggregate state
    // initializer is not supported under --evm-storage-layout, and the dep is
    // compiled with whatever mode the case runs in.
    function name() external pure returns (string memory) { return "%(name)s"; }
    function symbol() external pure returns (string memory) { return "%(sym)s"; }
    uint8 public constant decimals = %(dec)d;
    uint256 public totalSupply;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;
    mapping(address => mapping(address => bool)) public isApprovedForAll;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);

    function approve(address s, uint256 v) external returns (bool) {
        allowance[msg.sender][s] = v;
        emit Approval(msg.sender, s, v);
        return true;
    }

    function transfer(address t, uint256 v) external returns (bool) {
        balanceOf[msg.sender] -= v;
        balanceOf[t] += v;
        emit Transfer(msg.sender, t, v);
        return true;
    }

    function transferFrom(address f, address t, uint256 v) external returns (bool) {
        uint256 a = allowance[f][msg.sender];
        if (a != type(uint256).max) allowance[f][msg.sender] = a - v;
        balanceOf[f] -= v;
        balanceOf[t] += v;
        emit Transfer(f, t, v);
        return true;
    }

    function mint(address t, uint256 v) external {
        balanceOf[t] += v;
        totalSupply += v;
        emit Transfer(address(0), t, v);
    }

    function setApprovalForAll(address op, bool ok) external {
        isApprovedForAll[msg.sender][op] = ok;
    }
}
"""

_STUB_ABI = [
    {"type": "constructor", "inputs": []},
    {"type": "function", "name": "decimals", "inputs": [],
     "outputs": [{"type": "uint8", "name": ""}], "stateMutability": "view"},
    {"type": "function", "name": "approve",
     "inputs": [{"type": "address", "name": "s"}, {"type": "uint256", "name": "v"}],
     "outputs": [{"type": "bool", "name": ""}], "stateMutability": "nonpayable"},
    {"type": "function", "name": "setApprovalForAll",
     "inputs": [{"type": "address", "name": "op"}, {"type": "bool", "name": "ok"}],
     "outputs": [], "stateMutability": "nonpayable"},
]


def write_stub_dep(host: str, address: str, dep_dir) -> dict | None:
    """Stand-in ERC-20 for a ctor dependency we cannot compile.

    Only for addresses that really HAVE code — a plain EOA passed as an
    `owner`/`admin` argument must stay an EOA, or deploying an app at its
    address would change what the contract under test sees.
    """
    addr = address.lower()
    # Retry: public Blockscout times out under load, and a dropped probe here
    # silently skips the stand-in — the ctor then fails for what looks like a
    # modelling reason (this ate Polymarket's USDC on the first run).
    # Two ways to ask "is there code here", because neither alone is enough:
    # the address summary times out on heavily-used accounts (Polygon USDC,
    # reliably) and this Blockscout has no eth_getCode module, while the
    # smart-contracts endpoint answers only for VERIFIED contracts — but a 200
    # from it is itself proof of code. A dropped probe silently skips the
    # stand-in, and the ctor then fails for what looks like a modelling reason.
    has_code = None
    for attempt in range(2):
        try:
            has_code = bool(http_json(
                f"https://{host}/api/v2/addresses/{address}").get("is_contract"))
            break
        except Exception:
            time.sleep(1.0 * (2 ** attempt))
    if has_code is None:
        try:
            sc = http_json(f"https://{host}/api/v2/smart-contracts/{address}")
            has_code = bool(sc.get("source_code") or sc.get("name"))
        except Exception:
            has_code = None
    if has_code is None:
        print(f"[fetch] stand-in probe for {address[:10]}… failed after retries "
              f"— no stub, ctor may not deploy")
        return None
    if not has_code:
        return None
    name, sym, dec = "Stub", "STUB", 18
    try:                                   # real decimals when it is a token:
        tok = http_json(f"https://{host}/api/v2/tokens/{address}")
        name = (tok.get("name") or name)[:32]
        sym = (tok.get("symbol") or sym)[:16]
        dec = int(tok.get("decimals") or 18)
    except Exception:
        pass
    dep_dir.mkdir(parents=True, exist_ok=True)
    (dep_dir / "prepared.sol").write_text(
        _STUB_ERC20 % {"name": name, "sym": sym, "dec": dec})
    dep = {"address": addr, "name": "StubERC20", "compiler_version": "0.8.x",
           "abi": _STUB_ABI, "ctor_args_hex": "", "ctor_deps": [], "stub": True,
           "stub_for": {"name": name, "symbol": sym, "decimals": dec}}
    dump_json(dep_dir / "case.json", dep)
    return dep


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


def fetch_case(host: str, address: str, tag: str, max_txns: int = 300,
               internal: bool = False, internal_parents: int = 200,
               stub_deps: bool = False) -> dict:
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

    # INTERNAL CALLS (contract-to-contract traffic): merged into the same
    # ordered stream as the direct txns. For router-driven tokens this is most
    # of the real state evolution — without it the closed-world filter eats
    # every downstream txn (ena replayed 34/200, sqd 1/200).
    callees: dict[str, int] = {}
    if internal and txns:
        # `internal_parents` bounds the dominant cost: one rate-limited
        # raw-trace request per parent txn (~1 h at 200 on public Blockscout,
        # ~5 min at 60). Lower it to trade internal-call density for wall clock.
        ic = fetch_internal_calls(
            host, address, txns[0]["block"], txns[-1]["block"],
            {t["hash"].lower() for t in txns}, max_parents=internal_parents,
            callee_sink=callees)
        if ic:
            txns.extend(ic)
            txns.sort(key=lambda t: (t["block"], t.get("txindex", 0),
                                     t.get("trace_pos", -1)))
            txns = txns[:max_txns]
            n_ic = sum(1 for t in txns if t.get("internal"))
            print(f"[fetch] {tag}: +{n_ic} internal call(s) merged "
                  f"(router-driven traffic that txlist alone can't see)")

    _cs = sc.get("compiler_settings") or {}
    case = {
        "tag": tag, "host": host, "address": addr,
        # the contract's OWN verified settings — the oracle leg falls back to
        # these when a default (unoptimized, no-viaIR) compile fails, which is
        # what modern stack-heavy contracts (Permit2) require.
        "solc_settings": {"optimizer": _cs.get("optimizer"),
                          "viaIR": _cs.get("viaIR"),
                          "evmVersion": _cs.get("evmVersion")},
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
    # The contract's own outgoing calls, sampled from direct txns. Skipped when
    # internal-call fetching is off — both share the same rate-limited API.
    if internal and txns:
        try:
            harvest_callees(host, address,
                            [t["hash"] for t in txns if not t.get("internal")],
                            callees)
        except Exception as e:
            print(f"[fetch] {tag}: callee harvest failed ({str(e)[:60]}) — "
                  f"dependencies limited to ctor args + source literals")
    # hardcoded address literals in the source join the candidate set (the
    # memecoin pattern: UniswapV2Factory/Router baked in, zero ctor args)
    import re as _re
    literals = {("0x" + m.lower()) for m in
                _re.findall(r"0x([0-9a-fA-F]{40})\b", sc["source_code"])}
    # Callees first, most-called first: they are the evidenced dependencies,
    # while literals are merely addresses that appear in the text.
    ranked = [a for a, _ in sorted(callees.items(), key=lambda kv: -kv[1])][:8]
    if ranked:
        print(f"[fetch] {tag}: {len(callees)} callee(s) observed in traces, "
              f"trying top {len(ranked)} as dependencies")
    _ctor_addrs = set(_decode_ctor_addresses(abi, ctor_hex))
    for a in ranked + list(_ctor_addrs) + sorted(literals):
        depdir = case_dir / "deps" / f"dep_{a[2:10]}"
        d = fetch_dep(host, a, depdir, depth=2, seen=seen)
        if d:
            ctor_deps.append({"addr": a, "dir": f"deps/dep_{a[2:10]}"})
            print(f"[fetch] {tag}: dep {d['name']} @ {a[:10]}… fetched")
        elif stub_deps and a in _ctor_addrs:
            # CONSTRUCTOR args only: those are what a failing ctor calls. A
            # stand-in for a random call-arg address would put an app where the
            # contract expects an account.
            d = write_stub_dep(host, a, depdir)
            if d:
                ctor_deps.append({"addr": a, "dir": f"deps/dep_{a[2:10]}"})
                sf = d["stub_for"]
                print(f"[fetch] {tag}: dep @ {a[:10]}… NOT compilable — "
                      f"ERC-20 stand-in ({sf['symbol']}, {sf['decimals']} dec); "
                      f"identical on both legs, fidelity to chain NOT claimed")
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
    internal = "--internal" in argv
    if internal:
        argv.remove("--internal")
    stub_deps = "--stub-deps" in argv
    if stub_deps:
        argv.remove("--stub-deps")
    if len(argv) != 3:
        sys.exit(__doc__)
    fetch_case(argv[0], argv[1], argv[2], max_txns, internal,
               stub_deps=stub_deps)


if __name__ == "__main__":
    main()
