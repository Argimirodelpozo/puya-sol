#!/usr/bin/env python
"""EVM leg — runs under the tiny-fuzzing-oracle .evmvenv python.

  .evmvenv/bin/python evm_leg.py <case_dir> '<json opts>'

opts: {"max_txns": N, "skips": {"12": "reason"}, "snapshot_every": K,
       "pin_time": true}

Decodes the historical calldata via the verified ABI, builds the address
registry, replays the sequence on a fresh eth-tester chain (multi-sender, real
constructor args, best-effort historical timestamps), and converges the
closed-world filter: any txn whose local status disagrees with its historical
receipt status is skipped and the replay restarts (fast, in-process).

Writes: registry.json, calls.json, evm_results.json into the case dir.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import (ZERO, arg_content20, build_registry, canon_value,
                        dump_json, evm_sender_privkey, load_json, marker_for,
                        symbol)

import solcx
from eth_abi import decode as abi_decode
from eth_account import Account
from eth_utils import event_abi_to_log_topic, function_abi_to_4byte_selector
from web3 import Web3


# ── ABI helpers ────────────────────────────────────────────────────────────

def canonical_type(inp) -> str:
    t = inp["type"]
    if t.startswith("tuple"):
        inner = "(" + ",".join(canonical_type(c) for c in inp.get("components", [])) + ")"
        return inner + t[len("tuple"):]          # tuple[] / tuple[2] suffixes
    return t


def fn_sig(entry) -> str:
    return entry["name"] + "(" + ",".join(canonical_type(i) for i in entry["inputs"]) + ")"


def walk_addresses(value, inp, sink):
    """Collect every address-typed leaf value into sink."""
    t = inp["type"]
    if t.endswith("]"):
        elem = {**inp, "type": t[: t.rindex("[")]}
        for x in value or []:
            walk_addresses(x, elem, sink)
    elif t == "tuple":
        for x, c in zip(list(value), inp.get("components", [])):
            walk_addresses(x, c, sink)
    elif t == "address":
        sink.append(value.lower())


def markerize(value, inp, reg):
    t = inp["type"]
    if t.endswith("]"):
        elem = {**inp, "type": t[: t.rindex("[")]}
        return [markerize(x, elem, reg) for x in (value or [])]
    if t == "tuple":
        return [markerize(x, c, reg) for x, c in zip(list(value), inp.get("components", []))]
    if t == "address":
        return marker_for(reg, value)
    if isinstance(value, (bytes, bytearray)):
        return {"__b__": bytes(value).hex()}
    return value


# ── main ───────────────────────────────────────────────────────────────────

def main():
    case_dir = Path(sys.argv[1]).resolve()
    opts = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    max_txns = int(opts.get("max_txns", 10**9))
    ext_skips = {int(k): v for k, v in (opts.get("skips") or {}).items()}
    snap_every = int(opts.get("snapshot_every", 25))
    pin_time = bool(opts.get("pin_time", True))

    case = load_json(case_dir / "case.json")
    abi = case["abi"]
    txns = case["txns"][:max_txns]
    creator = case["creation"]["creator"]

    # function tables
    sel_map, fns = {}, {}
    for e in abi:
        if e.get("type") == "function":
            sig = fn_sig(e)
            fns[sig] = e
            sel_map[function_abi_to_4byte_selector(e).hex()] = (sig, e)
    ctor = next((e for e in abi if e.get("type") == "constructor"), None)
    ctor_inputs = (ctor or {}).get("inputs", [])
    ctor_vals = []
    if ctor_inputs and case["ctor_args_hex"]:
        ctor_vals = list(abi_decode([canonical_type(i) for i in ctor_inputs],
                                    bytes.fromhex(case["ctor_args_hex"])))

    # ── decode pass + base skips ──────────────────────────────────────────
    decoded = []                          # idx-aligned: (sig, entry, values) | None
    base_skip = {}
    for i, t in enumerate(txns):
        if t["value"] > 0:
            base_skip[i] = "value"; decoded.append(None); continue
        inp = (t["input"] or "0x").removeprefix("0x")
        if len(inp) < 8:
            base_skip[i] = "no-calldata"; decoded.append(None); continue
        hit = sel_map.get(inp[:8])
        if not hit:
            base_skip[i] = f"unknown-selector:{inp[:8]}"; decoded.append(None); continue
        sig, entry = hit
        try:
            vals = list(abi_decode([canonical_type(x) for x in entry["inputs"]],
                                   bytes.fromhex(inp[8:])))
        except Exception as e:
            base_skip[i] = f"decode-error:{str(e)[:40]}"; decoded.append(None); continue
        decoded.append((sig, entry, vals))

    # ── registry (stable across convergence: built from the whole window) ──
    senders = []
    for i, t in enumerate(txns):
        if i not in base_skip:
            senders.append(t["from"])
    arg_addrs = []
    for i, d in enumerate(decoded):
        if d:
            for v, inp in zip(d[2], d[1]["inputs"]):
                walk_addresses(v, inp, arg_addrs)
    for v, inp in zip(ctor_vals, ctor_inputs):
        walk_addresses(v, inp, arg_addrs)
    reg = build_registry(creator, senders, arg_addrs)
    dump_json(case_dir / "registry.json", reg)

    # ── concrete EVM forms + inverse fold ─────────────────────────────────
    sender_acct = {}                                  # registry idx -> (addr, priv)
    for a, i in reg["senders"].items():
        acct = Account.from_key(evm_sender_privkey(i))
        sender_acct[i] = (acct.address, evm_sender_privkey(i))

    def concrete(marker):                             # marker -> EVM 0x address
        # web3.py REQUIRES checksummed addresses — synthetic ones are built
        # lowercase, so every produced address goes through to_checksum_address.
        m = marker["__addr__"]
        if m == "Z":
            return Web3.to_checksum_address(ZERO)
        if m == "C":
            return None                               # filled per-run (accounts[0])
        if isinstance(m, int) and m in sender_acct:
            return Web3.to_checksum_address(sender_acct[m][0])
        if isinstance(m, int):
            return Web3.to_checksum_address("0x" + arg_content20(m).hex())
        return Web3.to_checksum_address(ZERO)         # '?…' unmapped — shouldn't occur

    def resolve(v):
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            c = concrete(v)
            return c if c is not None else _deployer[0]
        if isinstance(v, dict) and set(v) == {"__b__"}:
            return bytes.fromhex(v["__b__"])
        if isinstance(v, list):
            return [resolve(x) for x in v]
        return v

    # ── calls.json (transportable, marker-ised) ───────────────────────────
    calls = []
    for i, t in enumerate(txns):
        d = decoded[i]
        calls.append({
            "i": i, "hash": t["hash"], "ts": t["ts"], "hist_ok": t["hist_ok"],
            "sender": marker_for(reg, t["from"]) if d else None,
            "sig": d[0] if d else None,
            "args": [markerize(v, inp, reg) for v, inp in zip(d[2], d[1]["inputs"])] if d else None,
            "skip": base_skip.get(i),
        })
    getters = [{"sig": fn_sig(e), "outputs": e["outputs"]}
               for e in abi if e.get("type") == "function"
               and not e["inputs"] and e["outputs"]
               and e.get("stateMutability") in ("view", "pure")]
    snapshot_at = sorted({i for i in range(len(txns)) if (i + 1) % snap_every == 0}
                         | ({len(txns) - 1} if txns else set()))
    meta = {"ctor_args": [markerize(v, inp, reg) for v, inp in zip(ctor_vals, ctor_inputs)],
            "ctor_inputs": ctor_inputs,
            "getters": getters, "snapshot_at": snapshot_at,
            "fns": {s: {"inputs": e["inputs"], "outputs": e["outputs"]} for s, e in fns.items()},
            "n": len(txns)}

    # Candidate nested-mapping pairs (owner -> spender), taken from what the
    # replay actually does: each call's sender paired with its address args (plus
    # itself). Bounds the nested probe to O(txns) reads instead of O(symbols^2).
    pair_partners: dict = {}
    def _syms_in(v):
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            yield symbol(v["__addr__"])
        elif isinstance(v, list):
            for x in v:
                yield from _syms_in(x)
    for c in calls:
        if not c.get("sender") or not c.get("args"):
            continue
        s_sym = symbol(c["sender"]["__addr__"])
        seen_args = [t for a in c["args"] for t in _syms_in(a)]
        # sender <-> each address arg (covers approve/transferFrom, where the
        # owner IS the sender) AND arg <-> arg: `permit(owner, spender, ...)`
        # writes allow[owner][spender] with NEITHER as the sender, so pairing
        # only against the sender silently skipped every permit-created entry.
        for a_sym in [s_sym] + seen_args:
            partners = pair_partners.setdefault(a_sym, set())
            partners.add(a_sym)
            for t in [s_sym] + seen_args:
                partners.add(t)

    # ── compile once with solcx ───────────────────────────────────────────
    solcx.set_solc_version("0.8.26")
    mf = case.get("multifile")
    settings = {"evmVersion": "paris",
                "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object",
                                               "storageLayout"]}}}
    if mf:
        # Real file tree + the verification's own remappings — solc consumes
        # both natively via standard-json.
        root = case_dir / "src"
        sources = {rel: {"content": (root / rel).read_text()} for rel in mf["files"]}
        if mf["remappings"]:
            settings["remappings"] = mf["remappings"]
    else:
        sources = {"prepared.sol": {"content": (case_dir / "prepared.sol").read_text()}}
    out = solcx.compile_standard({"language": "Solidity", "sources": sources,
                                  "settings": settings})
    target = None
    for by_name in out["contracts"].values():
        for cname, cdata in by_name.items():
            if cname == case["name"]:
                target = cdata
    assert target, f"contract {case['name']} not in solc output"
    bytecode = target["evm"]["bytecode"]["object"]

    # event topic map for log decoding
    from web3._utils.events import get_event_data
    topic_map = {}
    for e in abi:
        if e.get("type") == "event" and not e.get("anonymous"):
            try:
                topic_map["0x" + event_abi_to_log_topic(e).hex()] = e
            except Exception:
                pass

    layout = target.get("storageLayout") or {"storage": [], "types": {}}

    def _decode_slot_bytes(raw: bytes, label: str, fold):
        if label == "address" or label.startswith("contract "):
            return fold("0x" + raw[-20:].hex())
        if label == "bool":
            return bool(raw[-1] if raw else 0)
        if label.startswith("uint"):
            return int.from_bytes(raw, "big")
        if label.startswith("int"):
            return int.from_bytes(raw, "big", signed=True)
        if label.startswith("bytes"):
            return "0x" + raw.hex()
        if label.startswith("enum"):
            return int.from_bytes(raw, "big")
        return "0x" + raw.hex()

    # SCALAR state vars: bounded (a handful of slots), so reading them after
    # every txn is cheap and yields true per-txn DELTAS — which localise a
    # divergence to the exact txn that caused it, and catch corruption in
    # variables that have NO public getter (invisible to getter snapshots).
    scalars = []
    for e in layout.get("storage") or []:
        t = (layout.get("types") or {}).get(e["type"], {})
        label = t.get("label", "")
        if t.get("encoding") != "inplace":
            continue                                   # string/bytes/dynamic
        if label.startswith(("mapping", "struct")) or label.endswith("]"):
            continue                                   # handled as maps below
        scalars.append((e["label"], int(e["slot"]), int(e.get("offset", 0)),
                        int(t.get("numberOfBytes", 32)), label))

    # MAPPINGS: read only the keys the registry knows about (bounded by the
    # window's senders/args) — never an enumeration. Value shapes handled:
    # scalar, nested mapping, STRUCT (inplace members) and dynamic ARRAY.
    _TY = layout.get("types") or {}

    def _is_addr_mapping(label):
        # Solidity >=0.8.18 allows NAMED mapping params, so the label can be
        # "mapping(address account => uint256)" — matching on "mapping(address =>"
        # silently skipped those maps (op_gov/_nonces).
        return bool(re.match(r"^mapping\(address\b", label or ""))

    def _value_shape(tid):
        """Classify a mapping's value type into something the reader can decode."""
        vt = _TY.get(tid, {})
        label, enc = vt.get("label", ""), vt.get("encoding")
        if _is_addr_mapping(label):
            return ("mapping", vt)
        if enc == "inplace" and vt.get("members"):
            return ("struct", vt)
        if enc == "dynamic_array":
            return ("array", vt)
        if enc == "inplace":
            return ("scalar", vt)
        return (None, vt)                    # string/bytes/other: not covered

    maps = []
    for e in layout.get("storage") or []:
        t = _TY.get(e["type"], {})
        if not _is_addr_mapping(t.get("label", "")):
            continue
        kind, vt = _value_shape(t.get("value"))
        if kind == "mapping":
            k2, vt2 = _value_shape(vt.get("value"))
            if k2 == "scalar":
                maps.append((e["label"], int(e["slot"]), 2, ("scalar", vt2)))
        elif kind in ("scalar", "struct", "array"):
            maps.append((e["label"], int(e["slot"]), 1, (kind, vt)))

    _deployer = [None]                                 # set per run

    # ── SSTORE TRACE (the in-process answer to debug_traceTransaction) ────
    # eth-tester exposes no tracer, but the EVM runs IN-PROCESS and every
    # SSTORE funnels through AccountDB.set_storage, so patching that one method
    # is cheaper and far more robust than decoding an opcode stream.
    # Caveat, stated honestly: a write made inside a frame that later REVERTS is
    # journalled away by py-evm but still seen here, so the trace OVER-reports.
    # That is the safe direction — it can only over-state a blind spot, never
    # hide one.
    _trace = {"sink": None, "addr": None, "txn": None}

    def _install_sstore_trace():
        from eth.db.account import AccountDB
        if getattr(AccountDB, "_chd_traced", False):
            return
        _orig = AccountDB.set_storage

        def traced(self, address, slot, value):
            t, sink = _trace["txn"], _trace["sink"]
            # `txn` is only set around .transact(), so the read-only .call()
            # preflight (which also hits set_storage on a throwaway state)
            # cannot pollute the trace.
            if t is not None and sink is not None and (
                    _trace["addr"] is None or bytes(address) == _trace["addr"]):
                sink.setdefault(t, {})[slot] = value
            return _orig(self, address, slot, value)

        AccountDB.set_storage = traced
        AccountDB._chd_traced = True


    # ── one replay run ────────────────────────────────────────────────────
    def run_once(skips):
        from eth_tester import EthereumTester, PyEVMBackend
        genesis = None
        if pin_time and txns:
            try:
                from eth_tester.backends.pyevm.main import get_default_genesis_params
                genesis = get_default_genesis_params(
                    {"timestamp": max(1, case["creation"]["ts"] or txns[0]["ts"]) - 60})
            except Exception:
                genesis = None
        tester = EthereumTester(PyEVMBackend(genesis_parameters=genesis)
                                if genesis else PyEVMBackend())
        w3 = Web3(Web3.EthereumTesterProvider(tester))
        a0 = w3.eth.accounts[0]
        _deployer[0] = a0
        # 1 ETH per sender: they only ever pay gas (value-bearing txns are
        # skipped). At 1000 ETH each the deployer's ~1M ETH ran dry past ~1000
        # distinct senders, which killed every deep (>1500 txn) replay.
        for i, (addr, priv) in sender_acct.items():
            tester.add_account(priv)
            w3.eth.send_transaction({"from": a0, "to": addr,
                                     "value": 10**18, "gas": 21000})

        C = w3.eth.contract(abi=abi, bytecode=bytecode)
        txh = C.constructor(*[resolve(m) for m in meta["ctor_args"]]).transact(
            {"from": a0, "gas": 12_000_000})
        rc = w3.eth.get_transaction_receipt(txh)
        caddr = rc["contractAddress"]
        if not caddr:
            # No contract address => the constructor reverted or ran out of gas.
            # Almost always an external dependency the ctor calls (router,
            # oracle) that doesn't exist on a bare local chain. Report it as a
            # scope skip rather than crashing on None downstream.
            raise SystemExit(
                f"constructor failed to deploy (status={rc.get('status')}, "
                f"gasUsed={rc.get('gasUsed')}) — ctor likely calls an external "
                f"contract; not replayable standalone")
        inst = w3.eth.contract(address=caddr, abi=abi)
        sstore_trace: dict = {}
        _install_sstore_trace()
        _trace.update(sink=sstore_trace, addr=bytes.fromhex(caddr[2:]), txn=None)

        inv = {a0.lower(): symbol("C"), caddr.lower(): symbol("self"),
               ZERO: symbol("Z")}
        for a, i in reg["senders"].items():
            inv[sender_acct[i][0].lower()] = symbol(i)
        for a, i in reg["args"].items():
            inv[("0x" + arg_content20(i).hex()).lower()] = symbol(i)

        def fold(addr):
            if addr is None:
                return None
            return inv.get(str(addr).lower(), f"?{str(addr).lower()}")

        def decode_logs(receipt):
            outl = []
            for lg in receipt.get("logs", []):
                topics = lg.get("topics", [])
                if not topics:
                    continue
                t0 = topics[0]
                t0 = "0x" + (t0.hex() if hasattr(t0, "hex") else str(t0)).removeprefix("0x")
                ev = topic_map.get(t0) or topic_map.get(t0.lower())
                if not ev:
                    continue
                try:
                    data = get_event_data(w3.codec, ev, lg)["args"]
                except Exception:
                    continue
                outl.append({"name": ev["name"],
                             "args": [canon_value(data[i2["name"]], i2["type"], fold,
                                                  i2.get("components"))
                                      for i2 in ev["inputs"]]})
            return outl

        # Slots the readers actually LOOK AT. Any slot the trace saw written but
        # that never appears here is, by definition, state the differ is blind
        # to — reported rather than silently ignored.
        seen_slots: dict = {}

        def read_scalars():
            out = {}
            for name, slot, off, nb, label in scalars:
                seen_slots[slot] = name
                w = bytes(w3.eth.get_storage_at(caddr, slot)).rjust(32, b"\0")
                out[name] = _decode_slot_bytes(w[32 - off - nb:32 - off], label, fold)
            return out

        def read_maps():
            """{mapname: {symbol: value}} over registry-known address keys only."""
            from eth_utils import keccak

            cur_name = [""]

            def word(slot_int):
                seen_slots[slot_int] = cur_name[0]
                return bytes(w3.eth.get_storage_at(caddr, slot_int)).rjust(32, b"\0")

            def read_at(slot_int, shape):
                """Decode a mapping VALUE living at `slot_int`.

                scalar -> value | struct -> [members] | array -> [elements].
                Returns None when the slot is untouched, so an absent entry is
                distinguishable from a present zero."""
                kind, vt = shape
                if kind == "scalar":
                    raw = word(slot_int)
                    if not any(raw):
                        return None
                    return _decode_slot_bytes(raw, vt.get("label", "uint256"), fold)
                if kind == "struct":
                    vals, seen = [], False
                    for m in vt.get("members") or []:
                        mt = _TY.get(m["type"], {})
                        nb, off = int(mt.get("numberOfBytes", 32)), int(m.get("offset", 0))
                        w = word(slot_int + int(m.get("slot", 0)))
                        if any(w):
                            seen = True
                        vals.append(_decode_slot_bytes(
                            w[32 - off - nb:32 - off], mt.get("label", "uint256"), fold))
                    return vals if seen else None
                if kind == "array":
                    n = int.from_bytes(word(slot_int), "big")
                    if not n or n > 512:            # sanity bound
                        return None if not n else f"<{n} elements>"
                    base = int.from_bytes(keccak(slot_int.to_bytes(32, "big")), "big")
                    et = _TY.get(vt.get("base"), {})
                    esz = int(et.get("numberOfBytes", 32))
                    ekind = ("struct" if et.get("members") else "scalar", et)
                    out_l = []
                    for i in range(n):
                        # elements pack only when the element fits a word
                        per = max(1, (esz + 31) // 32)
                        out_l.append(read_at(base + i * per, ekind))
                    return out_l
                return None

            syms = [(symbol("C"), a0), (symbol("Z"), ZERO)]
            syms += [(symbol(i), sender_acct[i][0]) for i in sender_acct]
            syms += [(symbol(i), "0x" + arg_content20(i).hex()) for i in reg["args"].values()]
            sym_addr = dict(syms)
            out = {}
            for name, slot, depth, vshape in maps:
                cur_name[0] = name
                got = {}
                for sym, addr in syms:
                    k = bytes.fromhex(str(addr)[2:].lower().rjust(40, "0")).rjust(32, b"\0")
                    s1 = keccak(k + slot.to_bytes(32, "big"))
                    if depth == 1:
                        # An unset entry is an all-zero slot; read_at returns None
                        # for that. (Testing the DECODED value would be wrong: an
                        # address folds to the symbol "«Z»", which is truthy.)
                        v = read_at(int.from_bytes(s1, "big"), vshape)
                        if v is not None:
                            got[sym] = v
                    else:                                  # nested: owner => spender
                        # Only pairs the replay actually touched. The full
                        # cartesian product is O(n^2) STORAGE READS (450 symbols
                        # => 200k reads), which made deep windows unrunnable.
                        for sym2 in pair_partners.get(sym, ()):
                            addr2 = sym_addr.get(sym2)
                            if addr2 is None:
                                continue
                            k2 = bytes.fromhex(str(addr2)[2:].lower().rjust(40, "0")).rjust(32, b"\0")
                            s2 = keccak(k2 + s1)
                            v = read_at(int.from_bytes(s2, "big"), vshape)
                            if v is not None:
                                got[f"{sym}->{sym2}"] = v
                # Record even when empty: an empty map that WAS read is a real
                # comparison (both sides empty), whereas a missing entry means no
                # coverage at all. Conflating them hid that op_gov/_balances was
                # never diffed.
                out[name] = got
            return out

        results, snapshots, mismatches = {}, {}, []
        storage_delta, prev_scalars = {}, read_scalars()
        for c in calls:
            i = c["i"]
            if c["skip"] or i in skips:
                pass
            else:
                sender = resolve(c["sender"])
                if pin_time:
                    try:
                        head = w3.eth.get_block("latest")["timestamp"]
                        if c["ts"] > head:
                            tester.time_travel(c["ts"])
                    except Exception:
                        pass
                fn_abi = fns[c["sig"]]
                fn = inst.get_function_by_signature(c["sig"])
                args = [resolve(a) for a in c["args"]]
                try:
                    ret = fn(*args).call({"from": sender, "gas": 8_000_000})
                except Exception as e:
                    results[i] = {"ok": False, "revert": str(e)[:160]}
                    if c["hist_ok"]:
                        # Historically succeeded but reverts locally => it read
                        # state we don't have (external contract / balance).
                        mismatches.append((i, "local-revert-but-hist-ok",
                                           f"{c['sig']} {str(e)[:110]}"))
                    continue
                if not c["hist_ok"]:
                    mismatches.append((i, "local-ok-but-hist-revert", c["sig"]))
                    continue
                _trace["txn"] = i
                try:
                    txh2 = fn(*args).transact({"from": sender, "gas": 8_000_000})
                finally:
                    _trace["txn"] = None
                rcpt = w3.eth.get_transaction_receipt(txh2)
                if rcpt["status"] != 1:
                    mismatches.append((i, "call-ok-transact-fail", c["sig"]))
                    continue
                outs = fn_abi["outputs"]
                rets = list(ret) if len(outs) > 1 else ([ret] if outs else [])
                results[i] = {"ok": True,
                              "ret": [canon_value(v, o["type"], fold, o.get("components"))
                                      for v, o in zip(rets, outs)],
                              "logs": decode_logs(dict(rcpt))}
            if i in meta["snapshot_at"]:
                snap = {}
                for g in meta["getters"]:
                    try:
                        gv = inst.get_function_by_signature(g["sig"])().call({"from": a0})
                        vs = list(gv) if len(g["outputs"]) > 1 else [gv]
                        snap[g["sig"]] = [canon_value(v, o["type"], fold, o.get("components"))
                                          for v, o in zip(vs, g["outputs"])]
                    except Exception as e:
                        snap[g["sig"]] = f"REVERT:{str(e)[:60]}"
                snapshots[str(i)] = snap
            if i in results:                        # only executed txns can change state
                cur = read_scalars()
                d = {k: [prev_scalars.get(k), v] for k, v in cur.items()
                     if prev_scalars.get(k) != v}
                if d:
                    storage_delta[str(i)] = d
                prev_scalars = cur
        storage = {"scalars": read_scalars(), "maps": read_maps()}
        # Attribute every traced write. Executed-txn writes only: a skipped txn
        # never ran, and a reverted one is journalled back.
        writes, blind = {}, {}
        for t, slots in sstore_trace.items():
            if t not in results or not results[t].get("ok"):
                continue
            names = sorted({seen_slots[sl] for sl in slots if sl in seen_slots})
            unknown = [sl for sl in slots if sl not in seen_slots]
            writes[str(t)] = {"n": len(slots), "names": names,
                              "unattributed": len(unknown)}
            for sl in unknown:
                blind.setdefault(str(sl), []).append(t)
        storage["writes"] = writes
        storage["blind_slots"] = {k: v[:5] for k, v in list(blind.items())[:40]}
        storage["blind_slot_count"] = len(blind)
        return (results, snapshots, mismatches, storage_delta, storage)

    # ── closed-world convergence ──────────────────────────────────────────
    # BATCH convergence: one pass collects every mismatch, all get skipped at
    # once, repeat until a pass is clean. (Per-mismatch restart was O(N) runs.)
    # Results after the first mismatch in a pass may be state-forked, so we
    # always take the results of the FINAL clean pass.
    skips = dict(ext_skips)
    iterations = 0
    while True:
        iterations += 1
        results, snapshots, mismatches, sdelta, smaps = run_once(skips)
        if not mismatches or iterations >= 8:
            break
        for idx, why, detail in mismatches:
            skips[idx] = f"closed-world:{why}"
        shown = "; ".join(f"#{i}:{w}:{d[:70]}" for i, w, d in mismatches[:3])
        print(f"[evm] converge pass {iterations}: +{len(mismatches)} skip(s)  {shown}",
              file=sys.stderr)

    for c in calls:                                    # persist final skip set
        if c["i"] in skips and not c["skip"]:
            c["skip"] = skips[c["i"]]
    dump_json(case_dir / "calls.json", {"meta": meta, "calls": calls})
    dump_json(case_dir / "evm_results.json",
              {"iterations": iterations,
               "skips": {str(k): v for k, v in skips.items()},
               "results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "storage_delta": sdelta,
               "storage": smaps})
    n_exec = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[evm] replayed {n_exec}/{len(calls)} txns "
          f"({n_ok} ok, {n_exec-n_ok} reverted, {len(calls)-n_exec} skipped, "
          f"{iterations} iteration(s))")


if __name__ == "__main__":
    main()
