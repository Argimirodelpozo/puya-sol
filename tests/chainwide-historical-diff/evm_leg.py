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
from chd_common import (ZERO, arg_content20, build_dep_tape_plans,
                        build_registry, bytes32_mapping_key_candidates,
                        canon_value,
                        call_without_consuming_tapes, dump_json,
                        evm_sender_privkey, load_json, marker_for,
                        probe_clock_target, replay_clock_targets, replay_epoch,
                        scale_value, sender_marker, symbol)
from chd_storage import (EvmStorageReader, KeyEvidence,
                         build_parameterized_getter_probes,
                         grouped_uncovered_slots)

import solcx
from eth_abi import decode as _abi_decode_strict
from eth_abi import decoding as _eth_abi_decoding
from eth_abi.exceptions import NonEmptyPaddingBytes
from eth_account import Account
from eth_utils import event_abi_to_log_topic, function_abi_to_4byte_selector
from web3 import Web3


# Decoder classes that police padding bytes. eth_abi's `strict=False` does NOT
# relax this check (measured on 5.2) — only silencing these does.
_PADDING_VALIDATORS = tuple(
    c for c in (
        getattr(_eth_abi_decoding, n, None) for n in (
            "SingleDecoder", "FixedByteSizeDecoder", "SignedIntegerDecoder",
            "SignedFixedDecoder", "ByteStringDecoder")
    ) if c is not None and "validate_padding_bytes" in vars(c)
)


def abi_decode(types, data):
    """Decode ABI data the way the CHAIN did, not the way eth_abi prefers.

    An `address` occupies 20 bytes of a 32-byte word and the EVM MASKS the
    upper bits, so calldata carrying dirty padding executes fine on chain —
    eth_abi rejects it with NonEmptyPaddingBytes. That was killing whole cases
    (pol, susde die before their first txn) and silently downgrading real
    transactions to `decode-error` skips.

    Strict FIRST, lenient only as a fallback, so ordinary calldata keeps every
    check; a dirty word costs one retry. The masking is what the EVM itself
    does, so the replayed argument is the value the contract actually saw.
    """
    try:
        return _abi_decode_strict(types, data)
    except NonEmptyPaddingBytes:
        saved = [(c, c.validate_padding_bytes) for c in _PADDING_VALIDATORS]
        for cls, _ in saved:
            cls.validate_padding_bytes = lambda self, value, padding_bytes: None
        try:
            return _abi_decode_strict(types, data)
        finally:
            for cls, fn in saved:
                cls.validate_padding_bytes = fn


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
    time_base = int(opts.get("time_base") or 0)

    case = load_json(case_dir / "case.json")
    abi = case["abi"]

    # ── constructor dependencies (fetched by fetch.py): topo order, children
    # first, deduped by address. Each entry carries its own abi/ctor blob.
    def load_deps():
        out = []
        def rec(dir_, spec):
            dep = load_json(dir_ / "case.json")
            for sub in dep.get("ctor_deps") or []:
                rec(dir_ / sub["dir"], sub)
            out.append({"addr": spec["addr"].lower(), "dir": dir_, "case": dep})
        for spec in ((case.get("ctor_deps") or [])
                     + (case.get("arg_deps") or [])):
            rec(case_dir / spec["dir"], spec)
        seen, topo = set(), []
        for d in out:
            if d["addr"] not in seen:
                topo.append(d); seen.add(d["addr"])
        return topo
    deps = load_deps()
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
        if t["value"] > 0 and scale_value(t["value"]) is None:
            # Only unrepresentable amounts are dropped now (> uint64 after
            # scaling); the rest are replayed with value on both legs.
            base_skip[i] = "value-too-wide"; decoded.append(None); continue
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
    # dep ctor args join the registry too (their owner/admin addresses etc.)
    for d in deps:
        dctor = next((e for e in d["case"]["abi"]
                      if e.get("type") == "constructor"), None)
        d["ctor_inputs"] = (dctor or {}).get("inputs", [])
        d["ctor_vals"] = []
        if d["ctor_inputs"] and d["case"].get("ctor_args_hex"):
            try:
                d["ctor_vals"] = list(abi_decode(
                    [canonical_type(i) for i in d["ctor_inputs"]],
                    bytes.fromhex(d["case"]["ctor_args_hex"])))
            except Exception:
                d["ctor_inputs"], d["ctor_vals"] = [], []
        for v, inp in zip(d["ctor_vals"], d["ctor_inputs"]):
            walk_addresses(v, inp, arg_addrs)
    reg = build_registry(creator, senders, arg_addrs)
    reg["deps"] = {d["addr"]: i for i, d in enumerate(deps)}
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

    _dep_local = {}                                   # hist addr → local addr

    def resolve(v):
        if isinstance(v, dict) and set(v) == {"__dep__"}:
            return Web3.to_checksum_address(_dep_local[v["__dep__"]])
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
            # SCALED once, here, so both legs move the same number and neither
            # can drift from the other's idea of msg.value (chd_common).
            "value": scale_value(t.get("value") or 0) or 0,
            "sender": sender_marker(reg, t["from"]) if d else None,
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
    fn_meta = {s: {"inputs": e["inputs"], "outputs": e["outputs"]}
               for s, e in fns.items()}
    probes = build_parameterized_getter_probes(abi, calls, fn_meta)
    meta = {"ctor_args": [markerize(v, inp, reg) for v, inp in zip(ctor_vals, ctor_inputs)],
            "dep_ctors": [{"addr": d["addr"],
                           "dir": str(d["dir"].relative_to(case_dir)),
                           "name": d["case"].get("name"),
                           "args": [markerize(v, inp, reg) for v, inp in
                                    zip(d["ctor_vals"], d["ctor_inputs"])]}
                          for d in deps],
            "ctor_inputs": ctor_inputs,
            "getters": getters, "snapshot_at": snapshot_at,
            "probes": probes, "fns": fn_meta,
            "n": len(txns)}

    # ── compile once with solcx ───────────────────────────────────────────
    # Use the compiler that produced the deployed bytecode. A fixed 0.8.26
    # oracle cannot compile newer exact pragmas and may not recognise their
    # verified EVM target (Polymarket V2 is solc 0.8.34 + Prague). Pre-0.8
    # cases deliberately pragma-relaxed by the fetcher remain on 0.8.26,
    # because exact old-solc arithmetic fidelity was explicitly surrendered.
    solc_version = "0.8.26"
    if not case.get("pragma_relaxed_from"):
        match = re.search(r"(?:v)?(0\.8\.\d+)",
                          str(case.get("compiler_version") or ""))
        if match:
            solc_version = match.group(1)
    try:
        solcx.set_solc_version(solc_version)
    except Exception:
        print(f"[evm] installing verified solc {solc_version}")
        solcx.install_solc(solc_version, show_progress=False)
        solcx.set_solc_version(solc_version)
    print(f"[evm] compiler oracle: solc {solc_version}")
    mf = case.get("multifile")
    settings = {"evmVersion": "paris",
                "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object",
                                               "evm.bytecode.linkReferences",
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
    def _compile(extra=None):
        st = dict(settings)
        if extra:
            st.update(extra)
        return solcx.compile_standard({"language": "Solidity", "sources": sources,
                                       "settings": st})
    try:
        out = _compile()
    except Exception as _e:
        # Modern stack-heavy contracts (Permit2) only compile with the settings
        # they were VERIFIED with — viaIR + optimizer. Retry with those rather
        # than applying them everywhere: the existing corpus keeps the exact
        # oracle it was validated against.
        _ss = case.get("solc_settings") or {}
        # viaIR as VERIFIED. This used to read `True if viaIR is not False`,
        # which turned a verified-without-viaIR contract (the field is null,
        # not false) into a viaIR build — the opposite of "the settings it was
        # verified with". Polymarket's CTFExchange compiles cleanly at
        # viaIR=False and dies with a YulException at viaIR=True, so the
        # oracle rejected a contract the real chain compiles fine.
        _fallback = {
            "optimizer": _ss.get("optimizer") or {"enabled": True, "runs": 200},
            "viaIR": bool(_ss.get("viaIR")),
        }
        if _ss.get("evmVersion"):
            _fallback["evmVersion"] = _ss["evmVersion"]
        print(f"[evm] default solc compile failed ({str(_e)[:70]}) — retrying "
              f"with the contract's verified settings "
              f"(viaIR={_fallback['viaIR']}, optimizer on)")
        try:
            out = _compile(_fallback)
        except Exception as _e2:
            # Last resort for a contract whose verification records no viaIR
            # flag but which needs it anyway (older/partial metadata).
            if _fallback["viaIR"]:
                raise
            print(f"[evm] verified settings also failed ({str(_e2)[:70]}) — "
                  f"retrying with viaIR forced on")
            out = _compile({**_fallback, "viaIR": True})
    target = None
    for by_name in out["contracts"].values():
        for cname, cdata in by_name.items():
            if cname == case["name"]:
                target = cdata
    assert target, f"contract {case['name']} not in solc output"
    target_bytecode = target["evm"]["bytecode"]
    bytecode = target_bytecode["object"]
    target_link_refs = target_bytecode.get("linkReferences") or {}

    # Verified flattened sources can retain externally-linked Solidity
    # libraries. solc leaves 20-byte ``__$...$__`` placeholders in creation
    # bytecode; web3 quite reasonably rejects that as non-hex. Keep the exact
    # link-reference offsets so each fresh eth-tester run can deploy the
    # libraries first and patch in their local addresses.
    link_libraries = {}

    def collect_link_library(source_name, library_name):
        key = (source_name, library_name)
        if key in link_libraries:
            return
        lib = out["contracts"][source_name][library_name]
        lib_bytecode = lib["evm"]["bytecode"]
        lib_refs = lib_bytecode.get("linkReferences") or {}
        link_libraries[key] = {
            "abi": lib.get("abi") or [],
            "bytecode": lib_bytecode["object"],
            "link_refs": lib_refs,
        }
        for nested_source, nested_libraries in lib_refs.items():
            for nested_library in nested_libraries:
                collect_link_library(nested_source, nested_library)

    for source_name, libraries in target_link_refs.items():
        for library_name in libraries:
            collect_link_library(source_name, library_name)

    def link_bytecode(obj, refs, addresses):
        linked = obj
        for source_name, libraries in refs.items():
            for library_name, positions in libraries.items():
                key = (source_name, library_name)
                if key not in addresses:
                    raise RuntimeError(
                        f"linked library not deployed: {source_name}:{library_name}")
                address_hex = addresses[key].removeprefix("0x")
                for pos in positions:
                    start = int(pos["start"]) * 2
                    length = int(pos["length"]) * 2
                    if length != len(address_hex):
                        raise RuntimeError(
                            f"unexpected link width for {source_name}:{library_name}")
                    linked = linked[:start] + address_hex + linked[start + length:]
        return linked

    # Event topic map for log decoding — over EVERY unit in the compilation,
    # not just the target contract's ABI: events emitted from LIBRARY helpers
    # (e741's libES20.emitApproval) log the library's topic, which the target
    # ABI may not declare; decoding against it alone silently dropped those
    # logs and produced phantom event divergences (AVM emitted, "EVM didn't").
    # topic0 → CANDIDATE LIST, disambiguated at decode time by topic count:
    # overloads sharing a signature (e741's ERC-20 Approval w/ 2 indexed args
    # vs ERC-721 Approval w/ 3) hash to the SAME topic0 — a single-entry map
    # let one clobber the other and every log of the losing shape was dropped
    # ("Expected 3 log topics. Got 2"). Sourced from every compiled unit
    # (library-declared events included) plus the verified ABI.
    from web3._utils.events import get_event_data
    topic_map: dict = {}

    def _add_event_abi(e):
        if e.get("type") != "event" or e.get("anonymous"):
            return
        try:
            key = "0x" + event_abi_to_log_topic(e).hex()
        except Exception:
            return
        cands = topic_map.setdefault(key, [])
        n_indexed = sum(1 for i2 in e.get("inputs", []) if i2.get("indexed"))
        if not any(sum(1 for i2 in c.get("inputs", []) if i2.get("indexed"))
                   == n_indexed for c in cands):
            cands.append(e)

    for by_name in out["contracts"].values():
        for cdata in by_name.values():
            for e in cdata.get("abi", []) or []:
                _add_event_abi(e)
    for e in abi:
        _add_event_abi(e)

    # dependency bytecodes (single-file, same relaxed settings)
    for d in deps:
        try:
            dout = solcx.compile_standard({
                "language": "Solidity",
                "sources": {"dep.sol": {"content":
                    (d["dir"] / "prepared.sol").read_text()}},
                "settings": {"evmVersion": "paris",
                             "outputSelection": {"*": {"*":
                                 ["evm.bytecode.object"]}}}})
            dtarget = None
            for by_name in dout["contracts"].values():
                for cname, cdata in by_name.items():
                    if cname == d["case"].get("name"):
                        dtarget = cdata
            d["bytecode"] = dtarget["evm"]["bytecode"]["object"] if dtarget else None
            if d["bytecode"] is None:
                raise RuntimeError("target contract not in solc output")
        except Exception as e:
            d["bytecode"] = None
            print(f"[evm] dep {d['case'].get('name')}: compile failed "
                  f"({str(e)[:80]})")
        if d["bytecode"] is None:
            _fb = d["dir"] / "stub_fallback.sol"
            if _fb.exists():
                try:
                    dout = solcx.compile_standard({
                        "language": "Solidity",
                        "sources": {"dep.sol": {"content": _fb.read_text()}},
                        "settings": {"evmVersion": "paris",
                                     "outputSelection": {"*": {"*":
                                         ["evm.bytecode.object"]}}}})
                    _sc = dout["contracts"]["dep.sol"]["StubERC20"]
                    d["bytecode"] = _sc["evm"]["bytecode"]["object"]
                    d["case"]["abi"] = d["case"].get("stub_abi") or d["case"]["abi"]
                    d["ctor_vals"], d["ctor_inputs"] = [], []
                    d["is_fallback_stub"] = True
                    print(f"[evm] dep {d['case'].get('name')}: using generic "
                          f"stand-in (real dep uncompilable on this leg)")
                except Exception as e2:
                    print(f"[evm] dep stub fallback also failed: {str(e2)[:80]}")

    layout = target.get("storageLayout") or {"storage": [], "types": {}}
    # Slot-mode AVM replays read storage through the SAME layout (see
    # chd_slot_reader) — persist it for the AVM leg (runs after this one).
    dump_json(case_dir / "storage_layout.json", layout)

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

    _deployer = [None]                                 # set per run

    # ── SSTORE TRACE (the in-process answer to debug_traceTransaction) ────
    # eth-tester exposes no tracer, but the EVM runs IN-PROCESS and every
    # SSTORE funnels through AccountDB.set_storage, so patching that one method
    # is cheaper and far more robust than decoding an opcode stream.
    # Caveat, stated honestly: a write made inside a frame that later REVERTS is
    # journalled away by py-evm but still seen here, so the trace OVER-reports.
    # That is the safe direction — it can only over-state a blind spot, never
    # hide one.
    _trace = {"sink": None, "addr": None, "txn": None,
              "suppress_addr": None}

    def _install_sstore_trace():
        from eth.db.account import AccountDB
        if getattr(AccountDB, "_chd_traced", False):
            return
        _orig = AccountDB.set_storage

        def traced(self, address, slot, value):
            # Proxy-runtime deployment executes the implementation constructor
            # to obtain its real runtime code (including immutables), but those
            # constructor SSTOREs belong to the implementation account and are
            # absent from proxy storage. Suppress only writes to the predicted
            # CREATE address during that one constructor transaction.
            if (_trace.get("suppress_addr") is not None
                    and bytes(address) == _trace["suppress_addr"]):
                return None
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
        epoch = replay_epoch(calls)
        genesis = None
        if pin_time and txns:
            try:
                from eth_tester.backends.pyevm.main import get_default_genesis_params
                start = time_base or (case["creation"]["ts"] or txns[0]["ts"])
                genesis = get_default_genesis_params(
                    {"timestamp": max(61, start) - 60,
                     # susde's ctor burns >12M gas legitimately; mainnet's block
                     # limit is 30M+, ours was the binding constraint.
                     "gas_limit": 60_000_000})
            except Exception:
                genesis = None
        tester = EthereumTester(PyEVMBackend(genesis_parameters=genesis)
                                if genesis else PyEVMBackend())
        w3 = Web3(Web3.EthereumTesterProvider(tester))

        # Py-EVM's pending block starts at wall time even when a historical
        # custom-genesis timestamp was requested. Without an explicit anchor,
        # the first funding transaction silently jumps there. Anchor before
        # any setup transaction; the exact replay base is finalized after the
        # target has been deployed.
        if pin_time:
            pending_ts = int(tester.get_block_by_number("pending")["timestamp"])
            requested = int(time_base or epoch or pending_ts)
            tester.time_travel(max(requested, pending_ts))

        # ── VM-level tape interception ────────────────────────────────────
        # Solidity calls view functions via STATICCALL; a stub whose fallback
        # WRITES (__idx++) reverts there with EMPTY data — which silently
        # killed every tape answer served to a view (morpho: owner()/market()
        # is the FIRST sub-call of nearly every txn → 197 skips). The oracle
        # leg therefore serves recorded answers at the VM: a python-side
        # counter picks the next answer per stub address — no EVM state, so
        # static context is irrelevant. The AVM leg keeps the on-chain stub
        # tape (no staticcall there; writes are legal).
        _vm_tape = {}      # code_address bytes20 -> list[bytes] answers
        _vm_selectors = {} # code_address bytes20 -> list[bytes4] selectors
        _vm_cursor = {}    # code_address bytes20 -> int
        _vm_limit = {}     # code_address bytes20 -> current transaction end
        _backend = tester.backend
        _vmclass = _backend.chain.get_vm().__class__
        _compclass = _vmclass._state_class.computation_class
        # dicts live ON the class so convergence re-runs (fresh backend,
        # already-patched class) rebind cleanly instead of serving the first
        # run's closures
        _compclass._chd_tape_dicts = (
            _vm_tape, _vm_selectors, _vm_cursor, _vm_limit)
        if not getattr(_compclass, "_chd_tape_patched", False):
            _orig_apply = _compclass.apply_computation.__func__
            def _tape_apply(cls, state, message, tc, **kw):
                _t, _s, _k, _l = getattr(
                    cls, "_chd_tape_dicts", ({}, {}, {}, {}))
                ca = bytes(getattr(message, "code_address", b"") or b"")
                tape = _t.get(ca)
                sel = bytes(message.data[:4])
                k = _k.get(ca, 0)
                selectors = _s.get(ca, ())
                if (tape is not None and k < _l.get(ca, 0)
                        and k < len(tape) and k < len(selectors)
                        and selectors[k] == sel):
                    ans = tape[k]
                    _k[ca] = k + 1
                    import os as _os
                    if _os.environ.get("CHD_TAPE_DEBUG"):
                        print(f"[evm] tape-serve {ca.hex()[:8]} k={k} sel="
                              f"{bytes(message.data[:4]).hex()} -> {ans.hex()[:80]}",
                              file=__import__('sys').stderr)
                    message.code = b""            # execute nothing, succeed
                    comp = _orig_apply(cls, state, message, tc, **kw)
                    comp.output = ans
                    return comp
                return _orig_apply(cls, state, message, tc, **kw)
            _compclass.apply_computation = classmethod(_tape_apply)
            _compclass._chd_tape_patched = True
        a0 = w3.eth.accounts[0]
        _deployer[0] = a0
        # Gas float plus whatever SCALED value this sender actually pays out.
        # At a flat 1000 ETH each the deployer's ~1M ETH ran dry past ~1000
        # distinct senders, which killed every deep (>1500 txn) replay, so the
        # float stays small and the value part is computed per sender.
        owed = {}
        for c2 in calls:
            if c2.get("skip"):
                continue
            sv = scale_value(c2.get("value") or 0)
            m = (c2.get("sender") or {}).get("__addr__")
            if sv and isinstance(m, int):
                owed[m] = owed.get(m, 0) + sv
        for i, (addr, priv) in sender_acct.items():
            tester.add_account(priv)
            w3.eth.send_transaction({"from": a0, "to": addr,
                                     "value": 10**18 + owed.get(i, 0),
                                     "gas": 21000})

        _dep_local.clear()
        for d in deps:
            if not d.get("bytecode"):
                continue
            # Some verified ABIs omit their constructor while the creation
            # txn clearly passed args (raft_pm's 5th dep: 3 decoded args,
            # "Expected '0', got '3'") — synthesize the entry from the decoded
            # input types so web3 encodes what the chain actually did.
            _dabi = d["case"]["abi"]
            if d["ctor_vals"] and not any(e.get("type") == "constructor"
                                          and e.get("inputs") for e in _dabi):
                _dabi = ([e for e in _dabi if e.get("type") != "constructor"]
                         + [{"type": "constructor", "stateMutability": "nonpayable",
                             "inputs": d["ctor_inputs"]}])
            Cd = w3.eth.contract(abi=_dabi, bytecode=d["bytecode"])
            dargs = [resolve(markerize(v, inp, reg)) for v, inp in
                     zip(d["ctor_vals"], d["ctor_inputs"])]
            dtx = Cd.constructor(*dargs).transact({"from": a0, "gas": 30_000_000})
            drc = w3.eth.get_transaction_receipt(dtx)
            if not drc.get("contractAddress") and not d.get("is_fallback_stub"):
                # Real dep's ctor reverted locally (its own deps are absent) —
                # fall back to the generic stand-in rather than dying.
                _fb = d["dir"] / "stub_fallback.sol"
                if _fb.exists():
                    dout = solcx.compile_standard({
                        "language": "Solidity",
                        "sources": {"dep.sol": {"content": _fb.read_text()}},
                        "settings": {"evmVersion": "paris",
                                     "outputSelection": {"*": {"*":
                                         ["evm.bytecode.object"]}}}})
                    _sc = dout["contracts"]["dep.sol"]["StubERC20"]
                    d["case"]["abi"] = d["case"].get("stub_abi") or d["case"]["abi"]
                    Cd = w3.eth.contract(abi=d["case"]["abi"],
                                         bytecode=_sc["evm"]["bytecode"]["object"])
                    dtx = Cd.constructor().transact({"from": a0, "gas": 30_000_000})
                    drc = w3.eth.get_transaction_receipt(dtx)
                    print(f"[evm] dep {d['case'].get('name')}: ctor reverted — "
                          f"generic stand-in deployed instead")
            if not drc.get("contractAddress"):
                raise SystemExit(f"dependency {d['case'].get('name')} failed to "
                                 f"deploy (status={drc.get('status')})")
            _dep_local[d["addr"]] = drc["contractAddress"]
            print(f"[evm] dep {d['case'].get('name')} @ {drc['contractAddress'][:10]}…")

        # Deploy solc-linked libraries in dependency order. CCTP's Message
        # library is the motivating case, but the loop also handles libraries
        # linked against other libraries in the same compilation.
        library_addresses = {}
        pending_libraries = dict(link_libraries)
        while pending_libraries:
            progressed = False
            for key, spec in list(pending_libraries.items()):
                required = {
                    (source_name, library_name)
                    for source_name, libraries in spec["link_refs"].items()
                    for library_name in libraries
                }
                if not required.issubset(library_addresses):
                    continue
                lib_code = link_bytecode(
                    spec["bytecode"], spec["link_refs"], library_addresses)
                Lib = w3.eth.contract(abi=spec["abi"], bytecode=lib_code)
                ltx = Lib.constructor().transact({"from": a0, "gas": 30_000_000})
                lrc = w3.eth.get_transaction_receipt(ltx)
                if not lrc.get("contractAddress"):
                    raise SystemExit(
                        f"linked library {key[0]}:{key[1]} failed to deploy")
                library_addresses[key] = lrc["contractAddress"]
                del pending_libraries[key]
                progressed = True
                print(f"[evm] linked library {key[1]} @ "
                      f"{lrc['contractAddress'][:10]}…")
            if not progressed:
                unresolved = ", ".join(f"{s}:{n}" for s, n in pending_libraries)
                raise SystemExit(f"cyclic or missing linked libraries: {unresolved}")

        # TWO-PHASE tape load: every stub (ctor- and arg-level) must exist
        # before answers are translated, because answers can NAME other deps.
        # mapping20: historical 20-byte content → this leg's full 32-byte word.
        _m20 = {}
        for _a, _i in reg["args"].items():
            _m20[bytes.fromhex(_a[2:])] = bytes(12) + arg_content20(_i)
        for _a, _i in reg["senders"].items():
            _m20[bytes.fromhex(_a[2:])] = bytes(12) + bytes.fromhex(
                sender_acct[_i][0][2:])
        if reg.get("creator"):
            _m20[bytes.fromhex(reg["creator"][2:])] = bytes(12) + bytes.fromhex(a0[2:])
        for _a, _loc in _dep_local.items():
            _m20[bytes.fromhex(_a[2:])] = bytes(12) + bytes.fromhex(_loc[2:])
        # Selector-aware, transaction-bounded plans keep a missing or reverted
        # call from shifting the dependency answers consumed by later calls.
        dep_plans = build_dep_tape_plans(case_dir, set(), _m20, calls=calls)
        _dep_seek = {}
        for d in deps:
            _plan = dep_plans.get(d["addr"].lower())
            if not _plan or d["addr"] not in _dep_local:
                continue
            _ca = bytes.fromhex(_dep_local[d["addr"]][2:].lower())
            _vm_tape[_ca] = _plan["answers"]
            _vm_selectors[_ca] = _plan["selectors"]
            print(f"[evm] dep tape ARMED at VM: "
                  f"{len(_plan['answers'])} answer(s) "
                  f"@ {d['addr'][:10]}…")
            import os as _os
            if _os.environ.get("CHD_TAPE_DEBUG"):
                print(f"[evm] tape-head {d['addr'][:10]} head="
                      + " | ".join(a.hex()[:48]
                                   for a in _plan["answers"][:3]))
            _dep_seek[d["addr"].lower()] = (_ca, _plan["bounds"])

        linked_target_bytecode = link_bytecode(
            bytecode, target_link_refs, library_addresses)
        C = w3.eth.contract(abi=abi, bytecode=linked_target_bytecode)
        _install_sstore_trace()

        def _deploy_target(contract):
            if (case.get("proxy") or {}).get("initializer"):
                from eth._utils.address import generate_contract_address
                nonce = w3.eth.get_transaction_count(a0)
                _trace["suppress_addr"] = generate_contract_address(
                    bytes.fromhex(a0[2:]), nonce)
            try:
                return contract.constructor(
                    *[resolve(m) for m in meta["ctor_args"]]).transact(
                    {"from": a0, "gas": 30_000_000})
            finally:
                _trace["suppress_addr"] = None

        txh = _deploy_target(C)
        rc = w3.eth.get_transaction_receipt(txh)
        caddr = rc["contractAddress"]
        if not caddr and int(rc.get("gasUsed") or 0) >= 29_000_000:
            # Burning the WHOLE gas limit is the EIP-170 signature, not a
            # revert (a revert refunds): unoptimised runtime code over 24 KB
            # makes CREATE fail this way. We compile without the optimizer by
            # default, so a large contract verified WITH it (moonbirds: 37
            # files, optimizer runs=200) cannot deploy. RETRY with the
            # contract's OWN verified settings — the ones the chain used, so
            # more faithful anyway. Only on failure, so working cases are
            # untouched.
            _ss2 = case.get("solc_settings") or {}
            if _ss2.get("optimizer") or _ss2.get("viaIR"):
                try:
                    _st2 = dict(settings)
                    _st2["optimizer"] = _ss2.get("optimizer") or {
                        "enabled": True, "runs": 200}
                    if _ss2.get("viaIR"):
                        _st2["viaIR"] = True
                    if _ss2.get("evmVersion"):
                        _st2["evmVersion"] = _ss2["evmVersion"]
                    _out2 = solcx.compile_standard({
                        "language": "Solidity", "sources": sources,
                        "settings": _st2})
                    _t2 = None
                    for _byname in _out2["contracts"].values():
                        for _cn, _cd in _byname.items():
                            if _cn == case.get("name"):
                                _t2 = _cd
                    if _t2 and _t2["evm"]["bytecode"]["object"]:
                        # NOTE: a distinct name — assigning `bytecode` here
                        # would make it a local of run_once and turn the
                        # earlier read into an UnboundLocalError.
                        _bc2_data = _t2["evm"]["bytecode"]
                        _bc2 = link_bytecode(
                            _bc2_data["object"],
                            _bc2_data.get("linkReferences") or {},
                            library_addresses)
                        print(f"[evm] ctor out of gas at 30M (EIP-170 shape) — "
                              f"recompiled with verified settings "
                              f"(optimizer={bool(_st2.get('optimizer'))}, "
                              f"viaIR={bool(_st2.get('viaIR'))}), retrying")
                        C = w3.eth.contract(abi=abi, bytecode=_bc2)
                        txh = _deploy_target(C)
                        rc = w3.eth.get_transaction_receipt(txh)
                        caddr = rc["contractAddress"]
                except Exception as _e2:
                    print(f"[evm] verified-settings retry failed: {str(_e2)[:120]}")
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
        deployment_time = int(
            w3.eth.get_block(rc["blockNumber"])["timestamp"])
        sstore_trace: dict = {}
        _install_sstore_trace()
        _trace.update(sink=sstore_trace, addr=bytes.fromhex(caddr[2:]), txn=None)

        inv = {a0.lower(): symbol("C"), caddr.lower(): symbol("self"),
               ZERO: symbol("Z")}
        for a, i in reg["senders"].items():
            inv[sender_acct[i][0].lower()] = symbol(i)
        for a, i in reg["args"].items():
            inv[("0x" + arg_content20(i).hex()).lower()] = symbol(i)
        for a, i in (reg.get("deps") or {}).items():
            if a in _dep_local:
                inv[_dep_local[a].lower()] = symbol(f"D{i}")

        def fold(addr):
            if addr is None:
                return None
            return inv.get(str(addr).lower(), f"?{str(addr).lower()}")

        def decode_logs(receipt):
            outl = []
            for lg in receipt.get("logs", []):
                # The AVM result decoder observes logs from the target app,
                # not logs emitted by inner dependency apps.  Ethereum puts
                # both in one receipt; comparing all of them manufactured
                # CCTP mint divergences from the USDC stand-in's Transfer
                # event.  Keep the differential scoped to the contract under
                # test.  Internal-library events still use caddr and remain.
                if str(lg.get("address") or "").lower() != caddr.lower():
                    continue
                topics = lg.get("topics", [])
                if not topics:
                    continue
                t0 = topics[0]
                t0 = "0x" + (t0.hex() if hasattr(t0, "hex") else str(t0)).removeprefix("0x")
                cands = topic_map.get(t0) or topic_map.get(t0.lower()) or []
                # Prefer the overload whose indexed-arg count matches the log.
                cands = sorted(cands, key=lambda c: sum(
                    1 for i2 in c.get("inputs", []) if i2.get("indexed"))
                    != len(topics) - 1)
                ev, data = None, None
                for cand in cands:
                    try:
                        data = get_event_data(w3.codec, cand, lg)["args"]
                        ev = cand
                        break
                    except Exception:
                        continue
                if ev is None:
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

        def read_typed_storage():
            """Walk every solc-declared container recursively.

            Key candidates come from typed replay evidence. No mapping depth,
            key ordering, struct shape, or array element type is selected here.
            """
            from eth_utils import keccak

            syms = {symbol("C"): bytes.fromhex(a0[2:]).rjust(32, b"\0"),
                    symbol("Z"): bytes(32)}
            syms.update({symbol(i): bytes.fromhex(addr[2:]).rjust(32, b"\0")
                         for i, (addr, _key) in sender_acct.items()})
            syms.update({symbol(i): arg_content20(i).rjust(32, b"\0")
                         for i in reg["args"].values()})
            syms.update({symbol(f"D{i}"):
                         bytes.fromhex(_dep_local[addr][2:]).rjust(32, b"\0")
                         for addr, i in (reg.get("deps") or {}).items()
                         if addr in _dep_local})
            extras = bytes32_mapping_key_candidates(
                calls, meta.get("fns") or {}, keccak)
            evidence = KeyEvidence(
                calls, meta.get("fns") or {}, syms, extras)
            written = {slot for slots in sstore_trace.values() for slot in slots}
            reader = EvmStorageReader(
                layout,
                lambda slot: w3.eth.get_storage_at(caddr, slot),
                evidence, keccak, written)
            typed = reader.read(fold)
            seen_slots.update(reader.seen)
            return typed, reader

        if pin_time:
            # The pending block is the earliest one the first replay entry can
            # occupy. Build one shared monotonic schedule from that base; the
            # orchestrator passes the exact result to the AVM leg.
            pending_ts = int(tester.get_block_by_number("pending")["timestamp"])
            effective_time_base = max(int(time_base or epoch or pending_ts),
                                      pending_ts)
            clock_by_index = replay_clock_targets(calls, effective_time_base)
        else:
            effective_time_base = 0
            clock_by_index = {}

        results, snapshots, mismatches, block_ts, block_no = {}, {}, [], {}, {}
        storage_delta, prev_scalars = {}, read_scalars()
        def _take_snap(i):
            # Mirror of the post-txn snapshot. Mismatch paths call this before
            # their `continue`, because a snapshot taken on ONE leg only reads
            # as N fake divergences (FLOKI deep window: local-ok-but-hist-
            # revert skipped the EVM snapshot at txn 399; the AVM leg took
            # its own, and the differ compared value against absence).
            if i not in meta["snapshot_at"]:
                return
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

        for c in calls:
            i = c["i"]
            if c["skip"] or i in skips:
                pass
            else:
                sender = resolve(c["sender"])
                if pin_time:
                    want = clock_by_index.get(i)
                    try:
                        head = w3.eth.get_block("latest")["timestamp"]
                        if want and want > head:
                            tester.time_travel(want)
                    except Exception:
                        pass
                    # The transaction occupies the NEXT block. Record that
                    # block, not the current head (which is one block early).
                    try:
                        blk = w3.eth.get_block("latest")
                        block_ts[str(i)] = want
                        # Local HEIGHT too: a contract storing block.number
                        # writes each leg's own chain height, and the differ
                        # can only absorb that skew if it knows the height at
                        # the writing txn (staup `_locked` = block.number + K).
                        block_no[str(i)] = int(blk["number"]) + 1
                    except Exception:
                        pass
                for _ca2, _bounds in _dep_seek.values():
                    start, end = _bounds.get(i, (0, 0))
                    _vm_cursor[_ca2] = start
                    _vm_limit[_ca2] = end
                fn_abi = fns[c["sig"]]
                fn = inst.get_function_by_signature(c["sig"])
                args = [resolve(a) for a in c["args"]]
                try:
                    ret = call_without_consuming_tapes(
                        lambda: fn(*args).call(
                            {"from": sender, "gas": 8_000_000,
                             "value": c.get("value") or 0},
                            block_identifier="pending"),
                        _vm_cursor)
                except Exception as e:
                    # Custom errors hide in e.data / e.args — surface the raw
                    # selector+payload hex, or triage stops at "reverted:".
                    _rd = getattr(e, "data", None)
                    if _rd is None and e.args:
                        _rd = next((a for a in e.args
                                    if isinstance(a, (bytes, str))
                                    and str(a).startswith(("0x", "b'"))), None)
                    _rds = (_rd.hex() if isinstance(_rd, bytes) else str(_rd or ""))[:80]
                    _rds += " raw=" + repr(getattr(e, "args", ""))[:140]
                    results[i] = {"ok": False, "revert": str(e)[:160]}
                    if c["hist_ok"]:
                        # Historically succeeded but reverts locally => it read
                        # state we don't have (external contract / balance).
                        mismatches.append((i, "local-revert-but-hist-ok",
                                           f"{c['sig']} {str(e)[:80]} data={_rds}"))
                    _take_snap(i)
                    continue
                if not c["hist_ok"]:
                    mismatches.append((i, "local-ok-but-hist-revert", c["sig"]))
                    _take_snap(i)
                    continue
                _trace["txn"] = i
                try:
                    txh2 = fn(*args).transact({"from": sender, "gas": 8_000_000,
                                               "value": c.get("value") or 0})
                finally:
                    _trace["txn"] = None
                rcpt = w3.eth.get_transaction_receipt(txh2)
                if pin_time:
                    actual_block = w3.eth.get_block(rcpt["blockNumber"])
                    actual_ts = int(actual_block["timestamp"])
                    if want is not None and actual_ts != int(want):
                        raise RuntimeError(
                            f"replay clock mismatch at {i}: scheduled {want}, "
                            f"mined {actual_ts}")
                    block_ts[str(i)] = actual_ts
                    block_no[str(i)] = int(rcpt["blockNumber"])
                if rcpt["status"] != 1:
                    mismatches.append((i, "call-ok-transact-fail", c["sig"]))
                    _take_snap(i)
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
        # Drive the probe phase to the shared instant before reading anything.
        # Two measured behaviours decide the target, and both are off by one
        # from the obvious guess: a `.call()` executes against LATEST (not
        # pending), and `time_travel(t)` leaves latest at t-1. Aiming AT the
        # instant therefore has the contract observe one second less than the
        # AVM leg does — which is the whole of Aave's probe divergence, a
        # uniform 1.48e-7 of accrual on every asset. Aim one past, then record
        # what the contract will actually see rather than what was requested.
        probe_time = probe_clock_target(clock_by_index)
        if pin_time and probe_time:
            try:
                tester.time_travel(probe_time + 1)
            except Exception:
                # Forward-only; already at or past the target is fine.
                pass
            probe_time = int(w3.eth.get_block("latest")["timestamp"])
        probe_results = {}
        for probe_index, probe in enumerate(meta.get("probes") or []):
            try:
                source_txn = probe.get("source_txn")
                if source_txn is not None:
                    for _ca2, bounds in _dep_seek.values():
                        start, end = bounds.get(int(source_txn), (0, 0))
                        _vm_cursor[_ca2] = start
                        _vm_limit[_ca2] = end
                fn = inst.get_function_by_signature(probe["sig"])
                args = [resolve(value) for value in probe.get("args") or []]
                value = call_without_consuming_tapes(
                    lambda: fn(*args).call({"from": a0}), _vm_cursor)
                values = (list(value) if len(probe["outputs"]) > 1
                          else [value])
                probe_results[str(probe_index)] = {
                    "ok": True,
                    "ret": [canon_value(v, output["type"], fold,
                                        output.get("components"))
                            for v, output in zip(values, probe["outputs"])]}
            except Exception as exc:
                probe_results[str(probe_index)] = {
                    "ok": False, "revert": str(exc)[:160]}

        storage, typed_reader = read_typed_storage()
        # Keep the inexpensive per-transaction scalar reader as the delta
        # source; the recursive reader's scalar result should be identical.
        storage["scalars"] = read_scalars()
        # Attribute every traced write. Executed-txn writes only: a skipped txn
        # never ran, and a reverted one is journalled back.
        writes, blind_int = {}, {}
        for t, slots in sstore_trace.items():
            if t not in results or not results[t].get("ok"):
                continue
            names = sorted({seen_slots[sl] for sl in slots if sl in seen_slots})
            unknown = [sl for sl in slots if sl not in seen_slots]
            writes[str(t)] = {"n": len(slots), "names": names,
                              "unattributed": len(unknown)}
            for sl in unknown:
                blind_int.setdefault(sl, []).append(t)
        storage["writes"] = writes
        # Persist every unresolved identity and a useful grouping. The former
        # 40-slot truncation made post-run ownership analysis impossible.
        storage["blind_slots"] = {
            str(slot): txns for slot, txns in sorted(blind_int.items())}
        storage["blind_slot_count"] = len(blind_int)
        storage["blind_slot_groups"] = grouped_uncovered_slots(
            blind_int, blind_int, calls)
        def canonical_raw_slot(slot):
            word = bytes(w3.eth.get_storage_at(caddr, slot)).rjust(32, b"\0")
            folded = fold("0x" + word[-20:].hex())
            return (folded if any(word) and not str(folded).startswith("?")
                    else int.from_bytes(word, "big"))
        storage["raw_slots"] = {
            str(slot): canonical_raw_slot(slot) for slot in sorted(blind_int)}
        roots = {}
        for slot, owner in typed_reader.seen.items():
            root = owner.split("[", 1)[0].split(".", 1)[0]
            item = roots.setdefault(root, {"slots_read": 0,
                                           "written_slots_read": 0})
            item["slots_read"] += 1
            item["written_slots_read"] += int(slot in typed_reader.written_slots)
        storage["coverage"] = roots
        return (results, snapshots, probe_results, mismatches,
                storage_delta, storage,
                block_ts, block_no, effective_time_base, deployment_time,
                probe_time)

    # ── closed-world convergence ──────────────────────────────────────────
    # BATCH convergence: one pass collects every mismatch, all get skipped at
    # once, repeat until a pass is clean. (Per-mismatch restart was O(N) runs.)
    # Results after the first mismatch in a pass may be state-forked, so we
    # always take the results of the FINAL clean pass.
    skips = dict(ext_skips)
    skip_details = {}
    iterations = 0
    while True:
        iterations += 1
        (results, snapshots, probes, mismatches, sdelta, smaps,
         block_ts, block_no, effective_time_base,
         deployment_time, probe_time) = run_once(skips)
        if not mismatches or iterations >= 8:
            break
        for idx, why, detail in mismatches:
            skips[idx] = f"closed-world:{why}"
            skip_details[str(idx)] = {"kind": why, "detail": detail}
        shown = "; ".join(f"#{i}:{w}:{d[:340]}" for i, w, d in mismatches[:3])
        print(f"[evm] converge pass {iterations}: +{len(mismatches)} skip(s)  {shown}",
              file=sys.stderr)

    for c in calls:                                    # persist final skip set
        if c["i"] in skips and not c["skip"]:
            c["skip"] = skips[c["i"]]
    dump_json(case_dir / "calls.json", {"meta": meta, "calls": calls})
    dump_json(case_dir / "evm_results.json",
              {"iterations": iterations,
               "skips": {str(k): v for k, v in skips.items()},
               "skip_details": skip_details,
               "results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "probes": probes,
               "storage_delta": sdelta,
               "storage": smaps,
               "block_ts": block_ts,
               "block_no": block_no,
               "time_base": effective_time_base,
               "deployment_time": deployment_time,
               "probe_time": probe_time})
    n_exec = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[evm] replayed {n_exec}/{len(calls)} txns "
          f"({n_ok} ok, {n_exec-n_ok} reverted, {len(calls)-n_exec} skipped, "
          f"{iterations} iteration(s))")


if __name__ == "__main__":
    main()
