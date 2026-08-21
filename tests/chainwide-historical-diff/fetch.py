#!/usr/bin/env python3
"""Fetch a verified contract's source + ABI + constructor args + ASCENDING txn
history from a Blockscout instance (keyless) into cases/<tag>/.

  python3 fetch.py <host> <address> <tag> [--max-txns N] [--relax-pre08]

Sources:
  /api/v2/smart-contracts/{addr}   verified source, ABI, compiler, ctor args
  /api?module=account&action=txlist&sort=asc   ascending history incl. creation
"""
from __future__ import annotations

import shutil
import sys
import time

from chd_common import (CASES, EVM_PY, ZERO, dump_json, http_json, load_json,
                        relax_pragma)


def _decode_ctor_addresses(abi, ctor_hex):
    """Recursively collect address-typed constructor values.

    Solidity constructors can nest addresses under arbitrary tuple/array
    combinations (for example ``tuple(address,address,...)`` in Polymarket
    V2). Preserve the ABI component tree while walking the values returned by
    ``eth_abi``; inspecting only the canonical type string loses tuple
    component types and silently drops every nested dependency.
    """
    import json as _json
    import subprocess
    ctor = next((e for e in abi if e.get("type") == "constructor"), None)
    if not ctor or not ctor.get("inputs") or not ctor_hex:
        return []
    script = (
        "import sys, json\n"
        "from eth_abi import decode\n"
        "specs, hexdata = json.loads(sys.argv[1]), sys.argv[2]\n"
        "def ctype(s):\n"
        "    t = s['type']\n"
        "    if t.startswith('tuple'):\n"
        "        return '(' + ','.join(ctype(c) for c in s.get('components', [])) + ')' + t[5:]\n"
        "    return t\n"
        "types = [ctype(s) for s in specs]\n"
        "vals = decode(types, bytes.fromhex(hexdata))\n"
        "out = []\n"
        "def walk(v, s):\n"
        "    t = s['type']\n"
        "    if t == 'address': out.append(v.lower())\n"
        "    elif t.endswith(']'):\n"
        "        base = dict(s)\n"
        "        base['type'] = t[:t.rindex('[')]\n"
        "        for x in v: walk(x, base)\n"
        "    elif t == 'tuple':\n"
        "        for x, c in zip(v, s.get('components', [])): walk(x, c)\n"
        "for v, s in zip(vals, specs): walk(v, s)\n"
        "print(json.dumps(out))\n")
    specs = ctor["inputs"]
    try:
        p2 = subprocess.run([str(EVM_PY), "-c", script,
                             _json.dumps(specs), ctor_hex],
                            capture_output=True, text=True, timeout=60)
        return _json.loads(p2.stdout.strip() or "[]")
    except Exception:
        return []


def _proxy_constructor_setup(smart_contract: dict) -> tuple[str | None, str | None]:
    """Return ``(initial_implementation, initializer_calldata)`` for a proxy.

    Blockscout exposes constructor values together with their ABI descriptors.
    Keep this proxy-family agnostic: identify the implementation by the
    constructor parameter name (``logic``/``implementation``) and the
    initializer as the sole non-empty dynamic ``bytes`` argument. This covers
    transparent/ERC1967 and immutable-admin variants without baking in one
    constructor shape or initializer signature. Ambiguity is rejected.
    """
    decoded = smart_contract.get("decoded_constructor_args") or []
    implementation = None
    initializers = []
    for item in decoded:
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            continue
        value, spec = item
        if not isinstance(spec, dict):
            continue
        typ = str(spec.get("type") or "")
        name = str(spec.get("name") or "").lower().lstrip("_")
        if typ == "address" and ("implementation" in name or "logic" in name):
            if isinstance(value, str) and value.startswith("0x") and len(value) == 42:
                implementation = value.lower()
        if typ == "bytes" and isinstance(value, str):
            value = value.lower()
            if value.startswith("0x") and len(value) >= 10:
                initializers.append(value)
    initializer = initializers[0] if len(initializers) == 1 else None
    return implementation, initializer


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
            call_type = act.get("callType") or "call"
            is_dependency_call = call_type in ("call", "staticcall")
            if (not is_dependency_call
                    or (act.get("from") or "").lower() != addr):
                continue
            tgt = (act.get("to") or "").lower()
            if tgt and tgt != addr and tgt != ZERO:
                sink[tgt] = sink.get(tgt, 0) + 1
        time.sleep(0.6)



def harvest_dep_answers(host: str, address: str, case_dir, txns, dep_addrs,
                        max_traces: int = 200,
                        extra_tapes: dict | None = None) -> None:
    """Record what dependencies historically ANSWERED, in call order — and
    discover which ARG-level callees deserve stand-ins of their own.

    One pass over the replayed txns' raw traces records the output of EVERY
    sub-call made by the contract under test. Ctor-dep answers become that
    stub's tape as before. Callees OUTSIDE the ctor-dep set (a vault or oracle
    passed as a call argument — morpho_alloc's `IMetaMorpho(vault).owner()`)
    are code-probed; the busiest few become `arg_deps`: a stand-in plus tape,
    deployed and loaded on both legs exactly like ctor deps. Addresses that
    also SEND txns are excluded — the legs need a signable account for them.
    """
    # A refetch must not silently reuse stand-ins/tapes from a previous wider
    # window when this pass observes no such dependency.
    for stale in (case_dir / "arg_deps.json", case_dir / "dep_tape.json"):
        stale.unlink(missing_ok=True)

    addr = address.lower()
    deps = {d.lower() for d in dep_addrs}
    senders = {(t.get("from") or "").lower() for t in txns}
    per_callee: dict = {}
    n = 0
    for t in txns:
        if t.get("internal") or n >= max_traces:
            continue
        h = t["hash"]
        tr = None
        for attempt in range(3):
            try:
                tr = http_json(f"https://{host}/api/v2/transactions/{h}/raw-trace",
                               timeout=25)
                break
            except Exception:
                time.sleep(1.0 * (2 ** attempt))
        n += 1
        if n % 25 == 0:
            print(f"[fetch] dep-answer harvest: {n} trace(s), "
                  f"{sum(len(v) for v in per_callee.values())} answer(s) across "
                  f"{len(per_callee)} callee(s)", flush=True)
        if not isinstance(tr, list):
            continue
        for e in tr:
            if not isinstance(e, dict) or e.get("type") != "call":
                continue
            act = e.get("action") or {}
            # A proxy's DELEGATECALL selects the code being replayed; it is
            # not an external dependency and must never become a stand-in.
            # Only calls with a distinct callee context need scripted answers.
            if ((act.get("callType") or "call") not in ("call", "staticcall")
                    or (act.get("from") or "").lower() != addr):
                continue
            to = (act.get("to") or "").lower()
            if not to or to == addr:
                continue
            out = ((e.get("result") or {}).get("output") or "0x").removeprefix("0x")
            word = out if len(out) <= 2048 and len(out) % 2 == 0 else None
            sel = (act.get("input") or "0x")[:10]
            per_callee.setdefault(to, []).append({"hash": h, "sel": sel,
                                                 "out": word})
        time.sleep(0.5)

    # merge the internal-txn answers (lifted from parent traces) and order
    # EVERY tape by the final window position — cursors are positional, so a
    # tape whose entries sit in harvest order instead of replay order serves
    # the wrong answers to every interleaved internal txn.
    for a, v in (extra_tapes or {}).items():
        per_callee.setdefault(a, []).extend(v)
    _order = {}
    for _i, _t in enumerate(txns):
        _order.setdefault((_t.get("hash") or "").lower(), _i)
    def _pos(e):
        _h = (e.get("hash") or "").lower()
        return _order.get(_h, _order.get(_h.split("#")[0], 10**9))
    for a in per_callee:
        per_callee[a].sort(key=_pos)
    tapes = {a: v for a, v in per_callee.items() if a in deps}
    # ARG-level candidates: EVERY non-sender callee with recorded answers.
    # A top-N cut proved wrong twice — whichever vault falls below the line
    # leaves its txns calling a codeless address (empty revert, closed-world).
    arg_deps = []
    cands = sorted(((a, v) for a, v in per_callee.items()
                    if a not in deps and a not in senders and len(v) >= 1),
                   key=lambda kv: -len(kv[1]))
    for a, v in cands:
        depdir = case_dir / "deps" / f"argdep_{a[2:10]}"
        d = write_stub_dep(host, a, depdir)
        if not d:
            continue
        arg_deps.append({"addr": a, "dir": f"deps/argdep_{a[2:10]}"})
        tapes[a] = v
        print(f"[fetch] arg-level stand-in @ {a[:10]}… ({len(v)} answer(s))")
    if arg_deps:
        dump_json(case_dir / "arg_deps.json", {"arg_deps": arg_deps})
    if tapes:
        dump_json(case_dir / "dep_tape.json", {"tapes": tapes})
        print("[fetch] dep answers scripted: "
              + ", ".join(f"{a[:10]}…×{len(v)}" for a, v in tapes.items()))

def fetch_internal_calls(host: str, address: str, block_lo: int, block_hi: int,
                         direct_hashes: set, max_parents: int = 200,
                         max_calls: int = 400, callee_sink: dict | None = None,
                         tape_sink: dict | None = None,
                         parent_hints: list | None = None,
                         coverage_sink: dict | None = None) -> list:
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
    # Some systems have a known parent contract whose direct window is already
    # fetched (CCTP MessageTransmitter/TokenMessenger -> TokenMinter). Seeding
    # those hashes avoids relying on event/token indexes that cannot see a
    # value-free call into a contract which emits no event of its own.
    parents, seen_p = [], set()
    for h, blk, txi, ts in parent_hints or []:
        h = (h or "").lower()
        if not h or h in seen_p or not (block_lo <= int(blk or 0) <= block_hi):
            continue
        seen_p.add(h)
        parents.append((h, int(blk or 0), int(txi or 0), int(ts or 0)))
    page = 1
    while len(parents) < max_parents:
        before_page = len(parents)
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
            # NOT excluded when h is a direct txn: a parent that also calls us
            # directly still carries internal sub-calls; the ROOT entry is
            # already dropped by the traceAddress check downstream.
            if not h or h in seen_p:
                continue
            seen_p.add(h)
            parents.append((h, int(r.get("blockNumber") or 0),
                            int(r.get("transactionIndex") or 0),
                            int(r.get("timeStamp") or 0)))
            if len(parents) >= max_parents:
                break
        if len(rows) < 1000 or len(parents) == before_page:
            if len(rows) == 1000 and len(parents) == before_page:
                print("[fetch] token-transfer index repeated a full page; "
                      "stopping pagination with the unique parents retained")
            break
        page += 1
        time.sleep(0.4)
    # NON-TOKEN contracts (oracles, registries, governance) never appear in
    # tokentx, but every state-changing call they receive tends to EMIT — and
    # the log index carries the parent tx hash. Same trick, different index;
    # together they cover token and non-token traffic. AaveOracle: 0 direct
    # txns, 0 tokentx parents, 118 logs across 72 parents.
    if len(parents) < max_parents:
        page = 1
        while len(parents) < max_parents:
            before_page = len(parents)
            try:
                d = http_json(f"https://{host}/api?module=logs&action=getLogs"
                              f"&address={address}"
                              f"&fromBlock={block_lo}&toBlock={block_hi}"
                              f"&page={page}&offset=1000")
            except Exception:
                break
            rows = d.get("result") or []
            if not isinstance(rows, list) or not rows:
                break
            for r in rows:
                h = r.get("transactionHash")
                if not h or h in seen_p or h.lower() in direct_hashes:
                    continue
                def _i(x):
                    try:
                        x = r.get(x) or "0"
                        return int(x, 16) if isinstance(x, str) and x.startswith("0x") else int(x)
                    except Exception:
                        return 0
                seen_p.add(h)
                parents.append((h, _i("blockNumber"), _i("transactionIndex"),
                                _i("timeStamp")))
                if len(parents) >= max_parents:
                    break
            if len(rows) < 1000 or len(parents) == before_page:
                if len(rows) == 1000 and len(parents) == before_page:
                    print("[fetch] log index repeated a full page; stopping "
                          "pagination with the unique parents retained")
                break
            page += 1
            time.sleep(0.4)
    parents.sort(key=lambda p: (p[1], p[2]))
    parents = parents[:max_parents]

    out = []
    dropped = 0
    dropped_parents = []
    processed = 0
    for processed, (h, blk, txi, ts) in enumerate(parents, 1):
        if len(out) >= max_calls:
            break
        # Public Blockscout rate-limits hard: a burst of raw-trace requests
        # comes back as 500s. Pace + retry with backoff, and COUNT what we
        # still lose — a silently-partial internal-call set would overstate
        # replay coverage.
        tr = None
        for attempt in range(5):
            try:
                tr = http_json(
                    f"https://{host}/api/v2/transactions/{h}/raw-trace",
                    timeout=15)
                break
            except Exception:
                time.sleep(1.0 * (2 ** attempt))
        if processed % 10 == 0 or processed == len(parents):
            print(f"[fetch] internal parents: {processed}/{len(parents)} traced, "
                  f"{len(out)} call(s) into target", flush=True)
        if tr is None:
            dropped += 1
            dropped_parents.append({"hash": h, "block": blk,
                                    "txindex": txi, "ts": ts})
            continue
        if not isinstance(tr, list):
            continue
        _pend_in, _pend_out = [], []
        for i, e in enumerate(tr):
            if not isinstance(e, dict) or e.get("type") != "call":
                continue
            act = e.get("action") or {}
            call_type = act.get("callType") or "call"
            is_dependency_call = call_type in ("call", "staticcall")
            # Calls OUT of the contract, harvested from traces we already
            # fetched. These are the external contracts it genuinely depends
            # on — the ones whose absence makes a txn revert locally and get
            # dropped by the closed-world filter. Far better targeted than
            # "every address that appears in an argument", which is mostly
            # transfer recipients.
            if (callee_sink is not None and is_dependency_call
                    and (act.get("from") or "").lower() == addr):
                tgt = (act.get("to") or "").lower()
                if tgt and tgt != addr and tgt != ZERO:
                    callee_sink[tgt] = callee_sink.get(tgt, 0) + 1
            # OUR outgoing sub-calls inside this parent — the answers internal
            # txns will need (the whole setAdmin era makes owner() calls that
            # only exist in parent traces). Attributed to their owning
            # internal entry after the walk, once all traceAddresses are seen.
            if (tape_sink is not None and is_dependency_call
                    and (act.get("from") or "").lower() == addr):
                _to = (act.get("to") or "").lower()
                if _to and _to != addr:
                    _out = ((e.get("result") or {}).get("output")
                            or "0x").removeprefix("0x")
                    _pend_out.append((tuple(e.get("traceAddress") or []), _to,
                                      (act.get("input") or "0x")[:10],
                                      _out if len(_out) <= 2048
                                      and len(_out) % 2 == 0 else None))
            if not e.get("traceAddress"):
                continue                  # root call == the direct txn
            if (act.get("to") or "").lower() != addr:
                continue
            if call_type != "call":
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
            _pend_in.append((tuple(e["traceAddress"]),
                             f"{h}#{'.'.join(str(x) for x in e['traceAddress'])}"))
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
        # attribute: an outgoing call belongs to the deepest IN-entry whose
        # traceAddress is a prefix of its own; root-owned (direct-txn) calls
        # have no owning IN-entry and are already covered by the direct
        # harvest.
        if tape_sink is not None:
            for _ta, _to, _sel, _outhex in _pend_out:
                _best = None
                for _ita, _iid in _pend_in:
                    if len(_ita) <= len(_ta) and _ta[:len(_ita)] == _ita:
                        if _best is None or len(_ita) > len(_best[0]):
                            _best = (_ita, _iid)
                if _best is not None:
                    tape_sink.setdefault(_to, []).append(
                        {"hash": _best[1], "sel": _sel, "out": _outhex})
        time.sleep(0.6)
    if dropped:
        print(f"[fetch] internal calls: {dropped}/{len(parents)} parent trace(s) "
              f"unavailable after retries — those calls are NOT in the replay")
    if coverage_sink is not None:
        coverage_sink.update({
            "parents_selected": len(parents),
            "parents_processed": processed,
            "parent_traces_unavailable": dropped,
            "unavailable_parents": dropped_parents,
            "calls_into_target": len(out),
            "max_parents": max_parents,
            "max_calls": max_calls,
        })
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
    uint8 internal constant __decimals = %(dec)d;
    uint256 internal __totalSupply;
    mapping(address => uint256) internal __balanceOf;
    mapping(address => mapping(address => uint256)) internal __allowance;
    mapping(address => mapping(address => bool)) public isApprovedForAll;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);

    function name() external returns (string memory) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return "%(name)s";
    }
    function symbol() external returns (string memory) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return "%(sym)s";
    }
    function decimals() external returns (uint8) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return __decimals;
    }
    function totalSupply() external returns (uint256) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return __totalSupply;
    }
    function balanceOf(address account) external returns (uint256) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return __balanceOf[account];
    }
    function allowance(address owner, address spender) external returns (uint256) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        return __allowance[owner][spender];
    }

    function approve(address s, uint256 v) external returns (bool) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        __allowance[msg.sender][s] = v;
        emit Approval(msg.sender, s, v);
        return true;
    }

    function transfer(address t, uint256 v) external returns (bool) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        __balanceOf[msg.sender] -= v;
        __balanceOf[t] += v;
        emit Transfer(msg.sender, t, v);
        return true;
    }

    function transferFrom(address f, address t, uint256 v) external returns (bool) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        uint256 a = __allowance[f][msg.sender];
        if (a != type(uint256).max) __allowance[f][msg.sender] = a - v;
        __balanceOf[f] -= v;
        __balanceOf[t] += v;
        emit Transfer(f, t, v);
        return true;
    }

    function mint(address t, uint256 v) external returns (bool) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        __balanceOf[t] += v;
        __totalSupply += v;
        emit Transfer(address(0), t, v);
        return true;
    }

    function setApprovalForAll(address op, bool ok) external {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) assembly { return(add(answer, 32), mload(answer)) }
        isApprovedForAll[msg.sender][op] = ok;
    }

    // Scripted ANSWER TAPE: the fetcher records what this dependency
    // historically returned to the contract under test, sub-call by sub-call,
    // and both legs load the same tape. The fallback consumes it in order, so
    // a read-dependent txn replays against the dependency's REAL answers
    // instead of a guess; when the tape runs dry (or an answer was not a
    // single word) it falls through to the self-address answer below.
    // Tape storage from slot-mode-safe primitives only: scalar pushes into a
    // word store plus per-answer (byteLen, wordStart) — `bytes[]` push is not
    // supported there. Answers are reassembled at fallback time into a fresh
    // buffer (the supported new-bytes + mstore pointer idiom).
    bytes32[] public __words;
    uint256[] public __lens;
    uint256[] public __wstart;
    bytes32[] public __selectors;
    uint256 public __idx;
    uint256 public __end;
    // Absolute tape addressing: the legs seek before every replayed txn, so
    // a locally-reverted txn (whose __idx bump rolls back with it) can never
    // shift the answers the NEXT txn reads. Without this, one bad txn
    // desynchronised the whole suffix.
    function __seek(uint256 k, uint256 end) external {
        __idx = k;
        __end = end;
    }
    function __load(
        bytes32[] calldata w,
        uint256[] calldata lens,
        bytes32[] calldata selectors
    ) external {
        require(lens.length == selectors.length);
        uint256 wi = 0;
        for (uint256 i = 0; i < lens.length; i++) {
            __wstart.push(__words.length);
            __lens.push(lens[i]);
            __selectors.push(selectors[i]);
            uint256 nw = (lens[i] + 31) / 32;
            for (uint256 j = 0; j < nw; j++) {
                __words.push(w[wi + j]);
            }
            wi += nw;
        }
    }

    // Catch-all: any UNKNOWN selector answers ONE WORD holding THIS stub's
    // own address. Returning zero was a dead end: ENS's ReverseClaimer does
    // `ens.owner(node)` and then CALLS the answer — address(0) "succeeds"
    // with empty returndata and the bytes32 decode reverts. Answering
    // ourselves closes the chain: every address-valued view resolves back to
    // the stub, which keeps answering. Word-decoded as a number this is a
    // large value; either way both legs see the SAME thing, so the differ's
    // verdict is unaffected. Fidelity to the real chain stays NOT claimed.
    function _scriptedAnswer() internal returns (bool, bytes memory) {
        if (
            __idx < __end &&
            __idx < __lens.length &&
            __selectors[__idx] == bytes32(msg.sig)
        ) {
            uint256 len = __lens[__idx];
            uint256 base = __wstart[__idx];
            __idx++;
            uint256 nw = (len + 31) / 32;
            bytes memory w = new bytes(nw * 32);
            for (uint256 j = 0; j < nw; j++) {
                bytes32 x = __words[base + j];
                assembly {
                    mstore(add(w, add(32, mul(j, 32))), x)
                }
            }
            assembly { mstore(w, len) }
            return (true, w);
        }
        return (false, new bytes(0));
    }

    fallback(bytes calldata) external payable returns (bytes memory) {
        (bool scripted, bytes memory answer) = _scriptedAnswer();
        if (scripted) return answer;
        return abi.encode(address(this));
    }
    receive() external payable {}
}
"""

_STUB_ABI = [
    {"type": "constructor", "inputs": []},
    {"type": "function", "name": "__load",
     "inputs": [{"type": "bytes32[]", "name": "w"},
                {"type": "uint256[]", "name": "lens"},
                {"type": "bytes32[]", "name": "selectors"}],
     "outputs": [], "stateMutability": "nonpayable"},
    {"type": "function", "name": "__seek",
     "inputs": [{"type": "uint256", "name": "k"},
                {"type": "uint256", "name": "end"}],
     "outputs": [], "stateMutability": "nonpayable"},
    {"type": "function", "name": "decimals", "inputs": [],
     "outputs": [{"type": "uint8", "name": ""}], "stateMutability": "view"},
    {"type": "function", "name": "approve",
     "inputs": [{"type": "address", "name": "s"}, {"type": "uint256", "name": "v"}],
     "outputs": [{"type": "bool", "name": ""}], "stateMutability": "nonpayable"},
    {"type": "function", "name": "setApprovalForAll",
     "inputs": [{"type": "address", "name": "op"}, {"type": "bool", "name": "ok"}],
     "outputs": [], "stateMutability": "nonpayable"},
    {"type": "function", "name": "mint",
     "inputs": [{"type": "address", "name": "to"},
                {"type": "uint256", "name": "value"}],
     "outputs": [{"type": "bool", "name": ""}],
     "stateMutability": "nonpayable"},
]


def refresh_stub_source(dep_dir: Path, dep: dict,
                        filename: str = "prepared.sol") -> None:
    """Regenerate a fetched stand-in from its persisted token metadata."""
    stub = dep.get("stub_for") or {}
    name = (stub.get("name") or dep.get("name") or "Stub")[:32]
    symbol = (stub.get("symbol") or "STUB")[:16]
    decimals = int(stub.get("decimals") or 18)
    (dep_dir / filename).write_text(
        _STUB_ERC20 % {"name": name, "sym": symbol, "dec": decimals})


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
    dep = {"address": addr, "name": "StubERC20", "compiler_version": "0.8.x",
           "abi": _STUB_ABI, "ctor_args_hex": "", "ctor_deps": [], "stub": True,
           "stub_for": {"name": name, "symbol": sym, "decimals": dec}}
    refresh_stub_source(dep_dir, dep)
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


# ── mid-history upgrade harvesting (proxy.md §1, README "Mid-history
# upgrades"). Detection is event-first: EIP-1967 proxies emit
# Upgraded(address indexed implementation) from the PROXY address, which
# catches ProxyAdmin-routed upgrades that txlist can never show (they are
# internal calls of a ProxyAdmin root txn). Direct upgradeTo/upgradeToAndCall
# root txns are scanned as a fallback for logless hosts.
UPGRADED_TOPIC = (
    "0xbc7cd75a20ee27fd9adebab32041f755214dbc6bffa90cc0225b39da2e5c2d3b"
)
UPGRADE_SELECTORS = {"0x3659cfe6": "upgradeTo",
                     "0x4f1ef286": "upgradeToAndCall"}


def _int(x) -> int:
    s = str(x or 0)
    return int(s, 16) if s.startswith("0x") else int(s)


def materialize_impl_source(host: str, impl: str, dest, relax_pre08: bool):
    """Verified source of an implementation → dest/ (prepared.sol or src/
    tree), same layout fetch_case materializes for a case. None if
    unverified."""
    try:
        sc = http_json(f"https://{host}/api/v2/smart-contracts/{impl}")
    except Exception:
        return None
    if not sc.get("source_code"):
        return None
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "prepared.sol").write_text(
        relax_pragma(sc["source_code"], pre08=relax_pre08))
    mf = None
    extra = sc.get("additional_sources") or []
    if extra:
        main_rel = str(sc.get("file_path") or "Main.sol").lstrip("/") or "Main.sol"
        tree = {main_rel: relax_pragma(sc["source_code"], pre08=relax_pre08)}
        for f in extra:
            tree[str(f["file_path"]).lstrip("/")] = relax_pragma(
                f.get("source_code", ""), pre08=relax_pre08)
        src_root = dest / "src"
        shutil.rmtree(src_root, ignore_errors=True)
        for rel, content in tree.items():
            p = src_root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        mf = {"main": main_rel, "files": sorted(tree),
              "remappings": [r.lstrip(":") for r in
                             (sc.get("compiler_settings") or {}).get(
                                 "remappings") or []]}
    return {"name": sc.get("name"),
            "compiler_version": sc.get("compiler_version"),
            "abi": sc.get("abi") or [],
            "ctor_args_hex": (sc.get("constructor_args") or ""
                              ).removeprefix("0x"),
            "multifile": mf}


def _upgrade_events(host: str, address: str) -> list[dict]:
    """Upgraded(address) log entries on the proxy, oldest first."""
    try:
        rows = http_json(
            f"https://{host}/api?module=logs&action=getLogs&fromBlock=0"
            f"&toBlock=latest&address={address}&topic0={UPGRADED_TOPIC}"
        ).get("result") or []
    except Exception:
        rows = []
    if not isinstance(rows, list):
        return []
    out = []
    for e in rows:
        # OZ proxies index the implementation (topics[1]); the older Zeppelin
        # AdminUpgradeabilityProxy (e.g. USDC) leaves it non-indexed in data.
        topics = e.get("topics") or []
        word = topics[1] if len(topics) > 1 and topics[1] else e.get("data")
        if not word or len(str(word)) < 42:
            continue
        out.append({
            "hash": e.get("transactionHash"),
            "impl": "0x" + str(word)[2:66][-40:].lower(),
            "block": _int(e.get("blockNumber")),
            "txindex": _int(e.get("transactionIndex")),
            "ts": _int(e.get("timeStamp")),
        })
    return sorted(out, key=lambda x: (x["block"], x["txindex"]))


def _proxy_calls_in_txn(host: str, address: str, txhash: str) -> list[str]:
    """Calldata of every external call INTO the proxy in one txn's trace —
    the upgradeToAndCall blob (whose bytes arg is the init calldata) plus any
    sibling config calls, same harvesting shape as gen_v2_config's
    creation-era pass."""
    out = []
    try:
        for entry in http_json(
                f"https://{host}/api/v2/transactions/{txhash}/raw-trace"):
            act = entry.get("action", {})
            inp = act.get("input") or ""
            if (act.get("to") or "").lower() != address.lower() or not inp:
                continue
            if (act.get("from") or "").lower() == address.lower():
                continue
            out.append(inp)
    except Exception:
        pass
    return out


def fetch_upgrades(host: str, address: str, tag: str, case_dir, txns: list,
                   creation: dict, relax_pre08: bool,
                   source_from: str | None = None) -> None:
    """Detect in-window implementation upgrades and materialize each era.

    Writes cases/<tag>/upgrades.json + upgrade_<i>/ source dirs. The joint
    config entries (init decode, marker form, AVM artifacts) come from
    gen_upgrades.py, which consumes this file.
    """
    addr = address.lower()
    events = _upgrade_events(host, address)
    # txlist fallback for direct (EOA-admin) proxies: root calls carrying the
    # upgrade selectors that the log scan missed.
    seen_hashes = {e["hash"] for e in events}
    for t in txns:
        inp = (t.get("input") or "").lower()
        if inp[:10] not in UPGRADE_SELECTORS or t["hash"] in seen_hashes:
            continue
        events.append({"hash": t["hash"],
                       "impl": "0x" + inp[10 + 24:10 + 64],
                       "block": int(t["block"]),
                       "txindex": int(t.get("txindex") or 0),
                       "ts": int(t["ts"])})
    events.sort(key=lambda x: (x["block"], x["txindex"]))
    if not events:
        return
    creation_block = int(creation.get("block") or 0)
    window_end = max((int(t["block"]) for t in txns), default=creation_block)
    # The replay's first era is the --source-from implementation, so every
    # event up to and including its FIRST attachment is the initial era —
    # the attachment can post-date the creation block (factory-deployed
    # proxies wire the implementation in a follow-up txn), and treating it
    # as an upgrade would double-replay initialize. Without --source-from,
    # fall back to the creation-block rule.
    first_era_end = -1
    if source_from:
        for i, e in enumerate(events):
            if e["impl"] == source_from.lower():
                first_era_end = i
                break
        if first_era_end < 0:
            print(f"[fetch] {tag}: ⚠ --source-from {source_from} never "
                  f"appears in the Upgraded history — era alignment unknown")
    initial = (events[: first_era_end + 1] if first_era_end >= 0
               else [e for e in events if e["block"] <= creation_block])
    if initial:
        print(f"[fetch] {tag}: initial implementation {initial[-1]['impl']} "
              f"(attached block {initial[-1]['block']})")
        if len(initial) > 1:
            print(f"[fetch] {tag}: ⚠ {len(initial) - 1} pre-source_from "
                  f"era(s) exist — history before block "
                  f"{initial[-1]['block']} ran DIFFERENT code")
    rest = events[len(initial):] if first_era_end >= 0 else [
        e for e in events if e["block"] > creation_block]
    in_window = [e for e in rest if e["block"] <= window_end]
    skipped = [e for e in rest if e["block"] > window_end]
    if skipped:
        print(f"[fetch] {tag}: {len(skipped)} upgrade(s) beyond the harvested "
              f"window (block > {window_end}) — not materialized")
    if not in_window:
        return
    upgrades = []
    for i, e in enumerate(in_window):
        dest = case_dir / f"upgrade_{i}"
        meta = materialize_impl_source(host, e["impl"], dest, relax_pre08)
        if meta is None:
            print(f"[fetch] {tag}: upgrade {i} impl {e['impl']} NOT verified "
                  f"— era recorded without source; replay cannot cross it")
            upgrades.append({**e, "dir": f"upgrade_{i}", "unverified": True})
            continue
        # The upgrade txn's sender gates the init call on both legs.
        sender = None
        try:
            ct = http_json(f"https://{host}/api/v2/transactions/{e['hash']}")
            sender = ((ct.get("from") or {}).get("hash") or "").lower() or None
        except Exception:
            pass
        upgrades.append({
            **e, "dir": f"upgrade_{i}", "sender": sender,
            "init_calldata": _proxy_calls_in_txn(host, address, e["hash"]),
            **meta,
        })
        print(f"[fetch] {tag}: upgrade {i} @ block {e['block']} → "
              f"{meta['name']} ({e['impl'][:10]}…) materialized")
        time.sleep(0.4)
    dump_json(case_dir / "upgrades.json", {"address": addr,
                                           "upgrades": upgrades})


def fetch_case(host: str, address: str, tag: str, max_txns: int = 300,
               internal: bool = False, internal_parents: int = 200,
               stub_deps: bool = False, script_deps: bool = False,
               script_traces: int = 200,
               relax_pre08: bool = False,
               parent_hints: list | None = None,
               source_from: str | None = None,
               scan_upgrades: bool = False,
               creation_override: dict | None = None) -> dict:
    case_dir = CASES / tag
    addr = address.lower()

    # A proxy constructor normally delegatecalls its implementation's
    # initializer. Our cross-VM model deploys that implementation directly, so
    # retain the delegatecall as a semantic setup call or both local legs begin
    # in an impossible, uninitialised state. Only use it when the selected
    # source was also the constructor implementation; later implementations
    # belong to the upgrade-era path below.
    proxy_initial_impl = None
    proxy_initializer = None
    if source_from:
        try:
            proxy_sc = http_json(
                f"https://{host}/api/v2/smart-contracts/{address}")
            proxy_initial_impl, proxy_initializer = _proxy_constructor_setup(proxy_sc)
        except Exception as e:
            print(f"[fetch] {tag}: proxy constructor metadata unavailable "
                  f"({str(e)[:60]}) — initializer not materialized")

    # 1. verified source + metadata. --source-from splits the two roles a
    # proxy fuses: SOURCE (+abi/compiler) from the implementation address,
    # HISTORY from `address` (the proxy, where the traffic and events live).
    # The proxy hop itself is never replayed — the implementation deploys
    # directly, mirroring how delegatecall-based systems are handled.
    sc = http_json(
        f"https://{host}/api/v2/smart-contracts/{source_from or address}")
    if not sc.get("source_code"):
        sys.exit(f"[fetch] {tag}: contract not verified on {host}")
    comp = sc.get("compiler_version") or ""
    if "0.8." not in comp and not relax_pre08:
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
                "txindex": int(t.get("transactionIndex") or 0),
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
        # Blockscout spells this field BOTH ways across versions; checking
        # only one silently yields block/ts 0, which poisons every downstream
        # deploy timestamp (min(creation)-1 = -1 → the oracle rejects a
        # negative uint64) — and the corpus looks fetched, just wrong.
        creation = {"creator": (ai.get("creator_address_hash") or "").lower() or None,
                    "hash": (ai.get("creation_tx_hash")
                             or ai.get("creation_transaction_hash")),
                    "ts": 0, "block": 0}
        # Factory-created contracts (CCTP v2's proxies) have NO txlist creation
        # row, and a zero block/ts poisons downstream deploy timestamps
        # (min(creation)-1 = -1 → the oracle rejects a negative uint64).
        # Resolve the real block/ts from the creation txn itself.
        if creation["hash"]:
            try:
                ct = http_json(
                    f"https://{host}/api/v2/transactions/{creation['hash']}")
                import datetime
                creation["ts"] = int(datetime.datetime.fromisoformat(
                    ct["timestamp"].replace("Z", "+00:00")).timestamp())
                creation["block"] = int(ct.get("block_number") or ct.get("block"))
                if not creation["creator"]:
                    creation["creator"] = (
                        (ct.get("from") or {}).get("hash") or "").lower() or None
            except Exception as e:
                print(f"[fetch] {tag}: creation txn lookup failed: {e}")
    # Some explorers omit creation metadata for contracts created inside a
    # factory transaction. Allow authoritative externally-resolved metadata
    # to fill that gap without baking a site-specific scraper into the fetcher.
    if creation_override:
        creation = {
            "creator": creation_override["creator"].lower(),
            "hash": creation_override["hash"].lower(),
            "ts": int(creation_override["ts"]),
            "block": int(creation_override["block"]),
        }
    if not creation.get("block") or not creation.get("ts"):
        print(f"[fetch] {tag}: ⚠ CREATION BLOCK/TS UNRESOLVED "
              f"(block={creation.get('block')} ts={creation.get('ts')}) — "
              f"replay deploy timestamps will be wrong; fix before replaying")

    # INTERNAL CALLS (contract-to-contract traffic): merged into the same
    # ordered stream as the direct txns. For router-driven tokens this is most
    # of the real state evolution — without it the closed-world filter eats
    # every downstream txn (ena replayed 34/200, sqd 1/200).
    callees: dict[str, int] = {}
    if internal:
        # `internal_parents` bounds the dominant cost: one rate-limited
        # raw-trace request per parent txn (~1 h at 200 on public Blockscout,
        # ~5 min at 60). Lower it to trade internal-call density for wall clock.
        # A contract with ZERO direct txns (AaveOracle: all its traffic is
        # governance-driven, so txlist is empty) still has internal history —
        # gating the merge on `txns` silently produced an empty case that
        # looked fetched. Window falls back to creation block → open-ended.
        # ALWAYS anchor at creation when known: morpho_alloc's entire admin
        # era (setAdmin via internal txns) sat in the 159k-block gap between
        # factory creation and its first EXTERNAL txn — starting the parent
        # scan at txns[0].block silently skipped all of it.
        _lo = (int((creation or {}).get("block") or 0)
               or (txns[0]["block"] if txns else 0))
        _hi = txns[-1]["block"] if txns else 99_999_999
        internal_tapes = {}
        internal_coverage = {}
        ic = fetch_internal_calls(
            host, address, _lo, _hi,
            {t["hash"].lower() for t in txns},
            max_parents=internal_parents, max_calls=max_txns,
            callee_sink=callees,
            tape_sink=internal_tapes if script_deps else None,
            parent_hints=parent_hints,
            coverage_sink=internal_coverage)
        if ic:
            txns.extend(ic)
            txns.sort(key=lambda t: (t["block"], t.get("txindex", 0),
                                     t.get("trace_pos", -1)))
            txns = txns[:max_txns]
            n_ic = sum(1 for t in txns if t.get("internal"))
            internal_coverage.update({
                "calls_retained": n_ic,
                "transactions_retained": len(txns),
                "transaction_limit": max_txns,
            })
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
    if source_from:
        case["proxy"] = {
            "implementation": source_from.lower(),
            "constructor_implementation": proxy_initial_impl,
        }
        if (proxy_initializer
                and (proxy_initial_impl is None
                     or proxy_initial_impl == source_from.lower())):
            # This delegatecall ran inside the successful creation transaction.
            # The creator is mapped exactly like every other historical sender.
            setup = {
                "hash": f"{creation['hash']}#proxy-constructor-initializer",
                "from": creation["creator"],
                "input": proxy_initializer,
                "value": 0,
                "hist_ok": True,
                "ts": int(creation["ts"]),
                "block": int(creation["block"]),
                "txindex": -1,
                "internal": True,
                "setup": "proxy-constructor-initializer",
            }
            case["txns"] = [setup, *txns]
            case["proxy"]["initializer"] = proxy_initializer
            print(f"[fetch] {tag}: proxy constructor initializer "
                  f"{proxy_initializer[:10]} materialized as setup call")
    if "0.8." not in comp:
        case["pragma_relaxed_from"] = comp
    if internal:
        case["fetch_coverage"] = {
            "internal": locals().get("internal_coverage") or {},
            "script_direct_trace_limit": script_traces if script_deps else None,
        }
    # Source layout. Single-file → prepared.sol. Multi-file (the majority of
    # modern verifications: ~86% of Base's popular ERC-20s) → materialise the
    # real file TREE plus the verification's remappings, which both legs can
    # consume natively (solc standard-json sources+remappings; puya-sol
    # --source per file + --import-path + --remapping).
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / "source.sol").write_text(sc["source_code"])
    (case_dir / "prepared.sol").write_text(
        relax_pragma(sc["source_code"], pre08=relax_pre08))
    extra = sc.get("additional_sources") or []
    if extra:
        main_rel = sc.get("file_path") or "Main.sol"
        def _rel(p):                     # may be absolute in the API payload
            return str(p).lstrip("/") or "Main.sol"
        main_rel = _rel(main_rel)
        tree = {main_rel: relax_pragma(sc["source_code"], pre08=relax_pre08)}
        for f in extra:
            tree[_rel(f["file_path"])] = relax_pragma(
                f.get("source_code", ""), pre08=relax_pre08)
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
    # Scripted dependency harvesting below walks these same traces and also
    # discovers callees.  Running both passes doubled the slowest Blockscout
    # work without adding evidence.
    if internal and txns and not script_deps:
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
    # Scripted runtime dependencies are modeled from their observed call
    # boundary below. They are not constructor dependencies and should not be
    # redeployed from implementation source merely because they were called.
    ranked = ([] if script_deps else
              [a for a, _ in sorted(callees.items(), key=lambda kv: -kv[1])])
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
            # A real dep that later fails to COMPILE or DEPLOY on a leg would
            # otherwise vanish silently (raft_r's PositionManager) or kill the
            # run (raft_pm's PriceFeed, ctor status=0). Ship a generic
            # stand-in alongside it as a last-resort, identical on both legs.
            _fb = depdir / "stub_fallback.sol"
            if not _fb.exists():
                _fb.write_text(_STUB_ERC20 % {"name": d.get("name") or "Dep",
                                              "sym": "STUB", "dec": 18})
                _dc = load_json(depdir / "case.json") or {}
                _dc["stub_abi"] = _STUB_ABI
                dump_json(depdir / "case.json", _dc)
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
    # Runtime dependencies do not imply constructor dependencies.  Contracts
    # such as CCTP's TokenMinter take the token address in calldata and call it
    # later; requiring a ctor dependency here observes USDC in the trace and
    # then silently throws that evidence away.  Always run scripted discovery
    # when requested.  ``harvest_dep_answers`` code-probes observed callees and
    # excludes senders, so a plain address argument cannot accidentally become
    # a deployed stand-in.
    if script_deps:
        try:
            harvest_dep_answers(host, address, case_dir, txns,
                                [d["addr"] for d in ctor_deps],
                                max_traces=script_traces,
                                extra_tapes=locals().get("internal_tapes") or {})
            _adp = case_dir / "arg_deps.json"
            if _adp.exists():
                case["arg_deps"] = (load_json(_adp) or {}).get("arg_deps") or []
        except Exception as e:
            print(f"[fetch] {tag}: dep-answer harvest failed ({str(e)[:60]}) — "
                  f"stand-ins stay self-address-only")
    # Mid-history upgrades: on by default for proxy fetches (--source-from
    # states the case IS a proxy), opt-in via --upgrades otherwise.
    if source_from or scan_upgrades:
        try:
            fetch_upgrades(host, address, tag, case_dir, txns, creation,
                           relax_pre08, source_from=source_from)
        except Exception as e:
            print(f"[fetch] {tag}: upgrade scan failed ({str(e)[:80]}) — "
                  f"window treated as single-era")
    dump_json(case_dir / "case.json", case)
    mf = case.get("multifile")
    print(f"[fetch] {tag}: {sc.get('name')} solc={comp[:12]} "
          f"{'MULTI-FILE('+str(len(mf['files']))+' files)' if mf else 'single-file'} "
          f"creator={(creation['creator'] or '?')[:10]}… txns={len(txns)} "
          f"ctor_hex={len(ctor_hex)//2}B → {case_dir}")
    return case


def recover_unavailable_parents(host: str, tag: str) -> None:
    """Retry only the parent traces explicitly missed by a completed fetch."""
    case_dir = CASES / tag
    case = load_json(case_dir / "case.json") or {}
    coverage = ((case.get("fetch_coverage") or {}).get("internal") or {})
    missing = coverage.get("unavailable_parents") or []
    if not missing:
        print(f"[fetch] {tag}: no unavailable parent traces")
        return

    recovered_tapes = {}
    recovery_coverage = {}
    recovered = fetch_internal_calls(
        host, case["address"],
        min(int(p["block"]) for p in missing),
        max(int(p["block"]) for p in missing),
        {t["hash"].lower() for t in case.get("txns") or []
         if not t.get("internal")},
        max_parents=len(missing), max_calls=10**9,
        tape_sink=recovered_tapes,
        parent_hints=[(p["hash"], p["block"], p["txindex"], p["ts"])
                      for p in missing],
        coverage_sink=recovery_coverage)

    txns = list(case.get("txns") or [])
    known = {t["hash"] for t in txns}
    txns.extend(t for t in recovered if t["hash"] not in known)
    txns.sort(key=lambda t: (t["block"], t.get("txindex", 0),
                             t.get("trace_pos", -1)))
    case["txns"] = txns

    # Rebuild scripting from the prior plan plus newly recovered subcalls.
    prior_tapes = ((load_json(case_dir / "dep_tape.json") or {}).get("tapes")
                   or {})
    for addr, entries in recovered_tapes.items():
        prior_tapes.setdefault(addr, []).extend(entries)
    harvest_dep_answers(
        host, case["address"], case_dir, txns,
        [d["addr"] for d in case.get("ctor_deps") or []],
        max_traces=len(txns), extra_tapes=prior_tapes)
    arg_doc = load_json(case_dir / "arg_deps.json") or {}
    case["arg_deps"] = arg_doc.get("arg_deps") or []

    unavailable = recovery_coverage.get("unavailable_parents") or []
    internal_count = sum(1 for t in txns
                         if t.get("internal") and not t.get("setup"))
    coverage.update({
        "parent_traces_unavailable": len(unavailable),
        "unavailable_parents": unavailable,
        "calls_into_target": internal_count,
        "calls_retained": internal_count,
        "transactions_retained": len(txns)
            - sum(1 for t in txns if t.get("setup")),
    })
    dump_json(case_dir / "case.json", case)
    print(f"[fetch] {tag}: recovered {len(recovered)} internal call(s); "
          f"{len(unavailable)} parent trace(s) remain unavailable")


def main():
    argv = list(sys.argv[1:])
    if len(argv) == 3 and argv[0] == "--recover-unavailable":
        recover_unavailable_parents(argv[1], argv[2])
        return
    if len(argv) == 2 and argv[0] == "--refresh-stubs":
        case_dir = CASES / argv[1]
        case = load_json(case_dir / "case.json") or {}
        refreshed = 0
        for spec in ((case.get("ctor_deps") or [])
                     + (case.get("arg_deps") or [])):
            dep_dir = case_dir / spec["dir"]
            dep = load_json(dep_dir / "case.json") or {}
            if dep.get("stub"):
                refresh_stub_source(dep_dir, dep)
                refreshed += 1
            elif dep.get("stub_abi"):
                refresh_stub_source(dep_dir, dep, "stub_fallback.sol")
                refreshed += 1
        print(f"[fetch] {argv[1]}: refreshed {refreshed} stand-in source(s)")
        return
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
    script_deps = "--script-deps" in argv
    if script_deps:
        argv.remove("--script-deps")
    script_traces = 200
    if "--script-traces" in argv:
        i = argv.index("--script-traces")
        script_traces = int(argv[i + 1]); del argv[i:i + 2]
    relax_pre08 = "--relax-pre08" in argv
    if relax_pre08:
        argv.remove("--relax-pre08")
    source_from = None
    if "--source-from" in argv:
        i = argv.index("--source-from")
        source_from = argv[i + 1]
        del argv[i:i + 2]
    scan_upgrades = "--upgrades" in argv
    if scan_upgrades:
        argv.remove("--upgrades")
    creation_override = None
    if "--creation" in argv:
        i = argv.index("--creation")
        raw = argv[i + 1]
        del argv[i:i + 2]
        parts = raw.split(",")
        if len(parts) != 4:
            sys.exit("[fetch] --creation expects HASH,BLOCK,UNIX_TS,CREATOR")
        creation_override = {
            "hash": parts[0], "block": int(parts[1]),
            "ts": int(parts[2]), "creator": parts[3],
        }
    parent_case_tags = []
    while "--parent-case" in argv:
        i = argv.index("--parent-case")
        parent_case_tags.append(argv[i + 1])
        del argv[i:i + 2]
    parent_case_selectors = []
    while "--parent-case-selector" in argv:
        i = argv.index("--parent-case-selector")
        spec = argv[i + 1]
        del argv[i:i + 2]
        parts = spec.rsplit(":", 2)
        limit = None
        if len(parts) == 3 and parts[2].isdigit():
            parent_tag, selector, limit_s = parts
            limit = int(limit_s)
        elif len(parts) == 2:
            parent_tag, selector = parts
        else:
            sys.exit("[fetch] --parent-case-selector expects "
                     "TAG:0xSELECTOR[:LIMIT]")
        selector = selector.lower()
        if len(selector) != 10 or not selector.startswith("0x"):
            sys.exit("[fetch] parent selector must be a 4-byte 0x selector")
        parent_case_selectors.append((parent_tag, selector, limit))
    internal_parents = 200
    if "--internal-parents" in argv:
        i = argv.index("--internal-parents")
        internal_parents = int(argv[i + 1]); del argv[i:i + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    parent_hints = []
    for parent_tag, selector, limit in ([(t, None, None) for t in parent_case_tags]
                                        + parent_case_selectors):
        parent = load_json(CASES / parent_tag / "case.json") or {}
        if not parent.get("txns"):
            sys.exit(f"[fetch] --parent-case {parent_tag}: no fetched txns")
        matched = 0
        for order, txn in enumerate(parent["txns"]):
            if txn.get("internal"):
                continue
            if selector and not (txn.get("input") or "").lower().startswith(selector):
                continue
            parent_hints.append((
                txn.get("hash"), int(txn.get("block") or 0),
                int(txn.get("txindex", order)), int(txn.get("ts") or 0)))
            matched += 1
            if limit is not None and matched >= limit:
                break
    fetch_case(argv[0], argv[1], argv[2], max_txns, internal,
               internal_parents=internal_parents, stub_deps=stub_deps,
               script_deps=script_deps, script_traces=script_traces,
               relax_pre08=relax_pre08,
               parent_hints=parent_hints,
               source_from=source_from,
               scan_upgrades=scan_upgrades,
               creation_override=creation_override)


if __name__ == "__main__":
    main()
