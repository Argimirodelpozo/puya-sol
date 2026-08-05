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

from chd_common import (CASES, EVM_PY, ZERO, dump_json, http_json, load_json,
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



def _stub_selectors() -> list:
    """4-byte selectors the stand-in handles itself (never reach the fallback)."""
    sigs = ["name()", "symbol()", "decimals()", "totalSupply()",
            "balanceOf(address)", "transfer(address,uint256)",
            "transferFrom(address,address,uint256)",
            "approve(address,uint256)", "allowance(address,address)",
            "mint(address,uint256)", "burn(address,uint256)",
            "__load(bytes32[],uint256[])"]
    try:
        from eth_utils import keccak
    except Exception:
        return []
    return ["0x" + keccak(text=s).hex()[:8] for s in sigs]


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
            if (act.get("from") or "").lower() != addr:
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
                   key=lambda kv: -len(kv[1]))[:12]
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
        # The stand-in answers these selectors from its OWN logic, so those
        # calls never reach the tape-playing fallback. Recording them would
        # shift every later answer by one — the tape must contain exactly the
        # calls the fallback will see.
        dump_json(case_dir / "dep_tape.json",
                  {"tapes": tapes, "stub_selectors": _stub_selectors()})
        print("[fetch] dep answers scripted: "
              + ", ".join(f"{a[:10]}…×{len(v)}" for a, v in tapes.items()))

def fetch_internal_calls(host: str, address: str, block_lo: int, block_hi: int,
                         direct_hashes: set, max_parents: int = 200,
                         max_calls: int = 400, callee_sink: dict | None = None,
                         tape_sink: dict | None = None) -> list:
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
        if len(rows) < 1000:
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
        _pend_in, _pend_out = [], []
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
            # OUR outgoing sub-calls inside this parent — the answers internal
            # txns will need (the whole setAdmin era makes owner() calls that
            # only exist in parent traces). Attributed to their owning
            # internal entry after the walk, once all traceAddresses are seen.
            if tape_sink is not None and (act.get("from") or "").lower() == addr:
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
    uint256 public __idx;
    // Absolute tape addressing: the legs seek before every replayed txn, so
    // a locally-reverted txn (whose __idx bump rolls back with it) can never
    // shift the answers the NEXT txn reads. Without this, one bad txn
    // desynchronised the whole suffix.
    function __seek(uint256 k) external {
        __idx = k;
    }
    function __load(bytes32[] calldata w, uint256[] calldata lens) external {
        uint256 wi = 0;
        for (uint256 i = 0; i < lens.length; i++) {
            __wstart.push(__words.length);
            __lens.push(lens[i]);
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
    fallback() external payable {
        if (__idx < __lens.length) {
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
            assembly {
                return(add(w, 32), len)
            }
        }
        assembly {
            mstore(0x00, address())
            return(0x00, 0x20)
        }
    }
    receive() external payable {}
}
"""

_STUB_ABI = [
    {"type": "constructor", "inputs": []},
    {"type": "function", "name": "__load",
     "inputs": [{"type": "bytes32[]", "name": "w"},
                {"type": "uint256[]", "name": "lens"}],
     "outputs": [], "stateMutability": "nonpayable"},
    {"type": "function", "name": "__seek",
     "inputs": [{"type": "uint256", "name": "k"}],
     "outputs": [], "stateMutability": "nonpayable"},
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
               stub_deps: bool = False, script_deps: bool = False) -> dict:
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
        ic = fetch_internal_calls(
            host, address, _lo, _hi,
            {t["hash"].lower() for t in txns}, max_parents=internal_parents,
            callee_sink=callees,
            tape_sink=internal_tapes if script_deps else None)
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
    if script_deps and ctor_deps:
        try:
            harvest_dep_answers(host, address, case_dir, txns,
                                [d["addr"] for d in ctor_deps],
                                extra_tapes=locals().get("internal_tapes") or {})
            _adp = case_dir / "arg_deps.json"
            if _adp.exists():
                case["arg_deps"] = (load_json(_adp) or {}).get("arg_deps") or []
        except Exception as e:
            print(f"[fetch] {tag}: dep-answer harvest failed ({str(e)[:60]}) — "
                  f"stand-ins stay self-address-only")
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
    script_deps = "--script-deps" in argv
    if script_deps:
        argv.remove("--script-deps")
    internal_parents = 200
    if "--internal-parents" in argv:
        i = argv.index("--internal-parents")
        internal_parents = int(argv[i + 1]); del argv[i:i + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    fetch_case(argv[0], argv[1], argv[2], max_txns, internal,
               internal_parents=internal_parents, stub_deps=stub_deps,
               script_deps=script_deps)


if __name__ == "__main__":
    main()
