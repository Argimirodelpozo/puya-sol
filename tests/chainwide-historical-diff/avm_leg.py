#!/usr/bin/env python3
"""AVM leg — runs under the system python3 (algosdk + framework).

  python3 avm_leg.py <case_dir> '<json opts>'

Compiles cases/<tag>/prepared.sol with puya-sol, deploys it on LocalNet with
the real (registry-mapped) constructor args, funds one deterministic Algorand
account per historical sender, then replays calls.json in order with the
matching sender for each txn.

True multi-sender: framework's call() reads `localnet.account` at call time, so
each call swaps it to the mapped sender account (and restores after).

Writes avm_results.json into the case dir.
"""
from __future__ import annotations

import base64
import hashlib
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))
sys.path.insert(0, str(HERE.parents[0] / "WIP" / "tiny-fuzzing-oracle"))

from chd_common import (ZERO, algo_sender_seed, arg_content20, canon_value,
                        dump_json, is_platform_limit, load_json, symbol)

from algosdk import encoding
from algosdk.transaction import PaymentTxn, wait_for_confirmation
from nacl.signing import SigningKey

from framework import Harness
from framework.localnet import LocalNet
from event_diff import decode_avm_logs


class _Acct:
    """Minimal stand-in for algokit's account object (address/private_key are
    the only attributes framework.call reads)."""
    def __init__(self, address, private_key):
        self.address = address
        self.private_key = private_key
        self.signer = None


def ensure_app_funded(algod, dispenser, app_addr, min_spare=20_000_000,
                      add=200_000_000):
    """Top the app account up when its spendable margin runs low.

    Every mapping entry the contract writes becomes a box, and each box RAISES
    the app account's minimum balance. On long historical replays the MBR
    eventually exceeds the deploy-time funding and every subsequent txn fails
    with "balance N below min M" — which looks exactly like a mass miscompile
    in the diff (it produced 664 bogus 'divergences' on a 1200-txn PEPE run
    before this existed)."""
    try:
        info = algod.account_info(app_addr)
    except Exception:
        return False
    if info.get("amount", 0) - info.get("min-balance", 0) >= min_spare:
        return False
    sp = algod.suggested_params()
    txid = algod.send_transaction(
        PaymentTxn(dispenser.address, sp, app_addr, add).sign(dispenser.private_key))
    wait_for_confirmation(algod, txid, 6)
    return True


def _dec_avm(raw, vtype, fold):
    """Decode an AVM state value using the arc56-declared Solidity type."""
    t = str(vtype or "")
    # Sized bytesN FIRST: the EVM leg renders a bytesN slot as "0x…" hex, and
    # without this a bytes32 state var fell through to the int decode and read
    # as 0 while the EVM leg read "0x000…0" — the same zero, reported as a
    # divergence (xerc20/_PERMIT_TYPEHASH_DEPRECATED_SLOT). Handled before the
    # int short-circuit because AVM global state may hold it as a uint.
    _m = re.match(r"^bytes(\d+)$", t)
    if _m:
        n = int(_m.group(1))
        b = (raw.to_bytes(n, "big") if isinstance(raw, int)
             else (raw or b""))
        return "0x" + b.rjust(n, b"\0").hex()
    if isinstance(raw, int):
        return raw
    b = raw or b""
    if t in ("address", "account"):
        return fold(b)
    if t in ("AVMString", "string"):
        return b.decode("utf-8", "replace")
    if t in ("bool",):
        return bool(int.from_bytes(b, "big")) if b else False
    if t.startswith(("uint", "int", "AVMUint", "biguint")):
        return int.from_bytes(b, "big") if b else 0
    if len(b) <= 32:
        # AVMBytes-declared numerics (the common case for puya-sol state): a
        # short blob is the big-endian value, so decode it as an int to be
        # comparable with the EVM slot read.
        return int.from_bytes(b, "big") if b else 0
    return "0x" + b.hex()


def read_avm_storage(algod, app_id, arc56, fold):
    """AVM state → {scalars: {var: value}, maps: {mapname: {symbol: value}}}.

    The arc56 spec declares state BY SOLIDITY VARIABLE NAME (global keys and box
    map prefixes), which is what makes name-keyed diffing against solc's
    storageLayout possible across two totally different storage models.
    Reading it also covers variables with NO public getter."""
    st = (arc56 or {}).get("state") or {}
    gkeys = (st.get("keys") or {}).get("global") or {}
    bmaps = (st.get("maps") or {}).get("box") or {}
    by_key = {base64.b64decode(v["key"]): (name, v.get("valueType"))
              for name, v in gkeys.items() if v.get("key")}

    scalars = {}
    try:
        info = algod.application_info(app_id)
        for kv in (info.get("params") or {}).get("global-state") or []:
            k = base64.b64decode(kv["key"])
            if k not in by_key:
                continue
            name, vt = by_key[k]
            val = kv.get("value") or {}
            raw = (val.get("uint") if val.get("type") == 2
                   else base64.b64decode(val.get("bytes") or ""))
            scalars[name] = _dec_avm(raw, vt, fold)
    except Exception as e:
        scalars["__error__"] = str(e)[:80]

    return {"scalars": scalars}



def _canon_abi(v):
    """algosdk decode -> plain JSON-able lists/ints for cross-leg comparison."""
    if isinstance(v, (list, tuple)):
        return [_canon_abi(x) for x in v]
    if isinstance(v, (bytes, bytearray)):
        return "0x" + bytes(v).hex()
    return v


def _abi_type_for(vtype, arc56):
    """arc56 valueType -> an ABI type string, resolving NAMED structs.

    Returns None for plain scalars (handled directly) and anything unresolvable.
    """
    t = str(vtype or "")
    if not t or t in ("AVMBytes", "AVMString", "AVMUint64", "address", "account"):
        return None
    structs = (arc56 or {}).get("structs") or {}
    if t in structs:
        fields = structs[t]
        def one(f):
            ft = f.get("type")
            return _abi_type_for(ft, arc56) or ft
        return "(" + ",".join(one(f) for f in fields) + ")"
    # already an ABI type (possibly with [] / [N] suffixes)
    base = t.rstrip("[]0123456789")
    if base in structs:
        return _abi_type_for(base, arc56) + t[len(base):]
    return t


def read_avm_maps(algod, app_id, arc56, syms, fold):
    """Box-backed mappings → {mapname: {symbol: value}}, KEY-ALIGNED with the
    EVM side so entries compare one-for-one.

    puya-sol derives a mapping entry's box name by hashing, mirroring EVM's
    keccak layout:   m[k]    -> sha256(k ‖ name)
                     m[a][b] -> sha256(b ‖ sha256(a ‖ name))
    (verified empirically against a deployed app's box set). So the names are
    computed FORWARD from the registry's known keys — the hash is one-way, but
    it never needs inverting. Candidates are tested against the enumerated box
    set first, so only real hits cost an API read.

    `syms` is {symbol: 32-byte AVM content} for every registry address.
    """
    bmaps = ((arc56 or {}).get("state") or {}).get("maps", {}).get("box") or {}
    # Report what the contract DECLARES, so the differ can flag any mapping that
    # ends up uncompared instead of silently counting it as clean.
    out = {"__declared__": sorted(bmaps)}
    try:
        have = {base64.b64decode(b["name"])
                for b in (algod.application_boxes(app_id).get("boxes") or [])}
    except Exception as e:
        return {"__error__": str(e)[:80]}

    def val_of(name_b, vtype):
        try:
            raw = base64.b64decode(
                (algod.application_box_by_name(app_id, name_b) or {}).get("value") or "")
        except Exception:
            return None
        if not raw:
            return 0
        # An address-valued mapping must fold to a registry SYMBOL, or it reads
        # as a huge raw integer and every entry looks divergent against the EVM
        # side (which folds). Numeric types stay integers.
        t = str(vtype or "")
        if t in ("address", "account") and len(raw) == 32:
            return fold(raw)
        # STRUCT / ARRAY values: arc56 gives a real ABI type ("(uint32,uint224)[]")
        # or a named struct resolved via arc56["structs"]. Decoding yields the
        # same member/element LIST the EVM reader produces, so they compare.
        abi_t = _abi_type_for(t, arc56)
        if abi_t is not None:
            try:
                from algosdk import abi as _abi
                return _canon_abi(_abi.ABIType.from_string(abi_t).decode(raw))
            except Exception:
                pass
        return int.from_bytes(raw, "big")

    def _nonempty(v):
        """Mirror the EVM reader's `any(raw)` test.

        A struct decodes to a LIST, and `[0]` is truthy while the EVM side
        reports an all-zero struct as absent — that asymmetry alone would
        manufacture a divergence for every zero-valued struct entry."""
        if v is None or v == 0 or v == "":
            return False
        if isinstance(v, list):
            return any(_nonempty(x) for x in v)
        return True

    matched = set()
    for mapname, mspec in bmaps.items():
        vtype = mspec.get("valueType")
        m = mapname.encode()
        got = {}
        for sym, k in syms.items():                       # depth 1
            nm = hashlib.sha256(k + m).digest()
            if nm in have:
                matched.add(nm)
                v = val_of(nm, vtype)
                if _nonempty(v):
                    got[sym] = v
        if not got:                                       # depth 2 (nested)
            for s1, k1 in syms.items():
                inner = hashlib.sha256(k1 + m).digest()
                for s2, k2 in syms.items():
                    nm = hashlib.sha256(k2 + inner).digest()
                    if nm in have:
                        matched.add(nm)
                        v = val_of(nm, vtype)
                        if _nonempty(v):
                            got[f"{s1}->{s2}"] = v
        out[mapname] = got          # keep empty maps: see evm_leg read_maps

    # COVERAGE, mirroring the EVM leg's blind-slot trace: boxes that exist on
    # chain but that NO forward-derived candidate name matched. Root boxes named
    # after a state variable are legitimate non-mapping state, so exclude them.
    roots = {k.encode() for k in
             (((arc56 or {}).get("state") or {}).get("keys", {}).get("box") or {})}
    roots |= {k.encode() for k in bmaps}
    stray = [b for b in have - matched if b not in roots]
    out["__unattributed_boxes__"] = len(stray)
    return out


def algo_account(i: int) -> _Acct:
    seed = algo_sender_seed(i)
    sk = SigningKey(seed)
    pub = bytes(sk.verify_key)
    priv_b64 = base64.b64encode(seed + pub).decode()
    return _Acct(encoding.encode_address(pub), priv_b64)


def main():
    case_dir = Path(sys.argv[1]).resolve()
    opts = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    case = load_json(case_dir / "case.json")
    reg = load_json(case_dir / "registry.json")
    cj = load_json(case_dir / "calls.json")
    meta, calls = cj["meta"], cj["calls"]
    ext_skips = set(int(k) for k in (opts.get("skips") or []))
    evm_layout = bool(opts.get("evm_layout"))

    mut = {}
    for e in case["abi"]:
        if e.get("type") == "function":
            sig = e["name"] + "(" + ",".join(
                _ctype(i) for i in e["inputs"]) + ")"
            mut[sig] = e.get("stateMutability", "")

    ln = LocalNet()
    dispenser = ln.account
    h = Harness(ln, case_dir / "out_avm")

    # ── deterministic sender accounts, funded from the dispenser ──────────
    accts = {i: algo_account(i) for i in reg["senders"].values()}
    used = {c["sender"]["__addr__"] for c in calls
            if c.get("sender") and isinstance(c["sender"].get("__addr__"), int)}
    algod = ln.algod
    # LocalNet mines a block PER TXN, so a once-fetched suggested_params goes
    # stale after ~1000 payments ("txn dead: round X outside of Y--Z") — which
    # broke every deep replay with >1000 senders. Refresh periodically.
    sp = algod.suggested_params()
    last, sent = None, 0
    for i, a in accts.items():
        if i not in used:
            continue
        if sent and sent % 200 == 0:
            sp = algod.suggested_params()
        txn = PaymentTxn(dispenser.address, sp, a.address, 5_000_000)
        last = algod.send_transaction(txn.sign(dispenser.private_key))
        sent += 1
    if last:
        wait_for_confirmation(algod, last, 8)
    print(f"[avm] funded {len(used)} sender account(s)")

    # ── marker → AVM concrete value ───────────────────────────────────────
    def concrete_addr(m):
        if m == "Z":
            return encoding.encode_address(bytes(32))
        if m == "C":
            return dispenser.address
        if isinstance(m, int) and m in accts:
            return accts[m].address
        if isinstance(m, int):
            return encoding.encode_address(bytes(12) + arg_content20(m))
        return encoding.encode_address(bytes(32))

    def resolve(v):
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            return concrete_addr(v["__addr__"])
        if isinstance(v, dict) and set(v) == {"__b__"}:
            return bytes.fromhex(v["__b__"])
        if isinstance(v, list):
            return [resolve(x) for x in v]
        return v

    # ── compile + deploy ──────────────────────────────────────────────────
    mf = case.get("multifile")
    if mf:
        # compile_sol REMOVES import_dir when it finishes (normally a temp dir
        # made by the upstream splitter), so hand it a throwaway COPY — passing
        # cases/<tag>/src directly makes the compiler delete the fetched sources.
        tmp_root = Path(tempfile.mkdtemp(prefix="chd_src_"))
        shutil.copytree(case_dir / "src", tmp_root, dirs_exist_ok=True)
        artifacts = h.compile(tmp_root / mf["main"],
                              extra_sources=[tmp_root / r for r in mf["files"]],
                              extra_import_dir=tmp_root,
                              extra_remappings=mf["remappings"],
                              extra_args=(["--evm-storage-layout"] if evm_layout else None))
    else:
        artifacts = h.compile(case_dir / "prepared.sol",
                              extra_args=(["--evm-storage-layout"] if evm_layout else None))
    app = h.deploy(artifacts, case["name"],
                   ctor_args=[resolve(m) for m in meta["ctor_args"]] or None)
    print(f"[avm] deployed {case['name']} app_id={app.app_id}")
    ensure_app_funded(algod, dispenser, app.app_addr)     # headroom for box MBR
    topups = 0

    # inverse fold: 32-byte content hex → registry symbol
    inv = {encoding.decode_address(dispenser.address).hex(): symbol("C"),
           encoding.decode_address(app.app_addr).hex(): symbol("self"),
           bytes(32).hex(): symbol("Z")}
    for i, a in accts.items():
        inv[encoding.decode_address(a.address).hex()] = symbol(i)
    for _a, i in reg["args"].items():
        inv[(bytes(12) + arg_content20(i)).hex()] = symbol(i)

    def fold(v):
        if v is None:
            return None
        if isinstance(v, str) and v.startswith("0x"):
            hx = v[2:].lower()
        elif isinstance(v, (list, tuple, bytes, bytearray)):
            hx = bytes(v).hex()
        elif isinstance(v, str):
            try:
                hx = encoding.decode_address(v).hex()
            except Exception:
                return f"?{v}"
        else:
            return f"?{v}"
        return inv.get(hx, f"?0x{hx}")

    avm_events = getattr(app.app_spec, "events", None) or []
    ev_types = {e["name"]: e["inputs"] for e in case["abi"] if e.get("type") == "event"}

    def _canon_arg(v, sol_type):
        """Match the EVM leg's canonical form for one event argument.

        address -> registry symbol; bytesN/bytes -> "0x…" hex. algosdk decodes a
        `byte[32]` to a LIST OF INTS, which never equals the EVM leg's hex
        string even when the bytes are identical — that alone reported 4 bogus
        divergences on temple (OZ AccessControl role hashes)."""
        t = str(sol_type or "")
        if t == "address":
            return fold(v)
        if t.startswith("bytes"):
            if isinstance(v, (list, tuple)):
                return "0x" + bytes(v).hex()
            if isinstance(v, (bytes, bytearray)):
                return "0x" + bytes(v).hex()
            return v
        if isinstance(v, (list, tuple)):          # arrays/tuples: element-wise
            base = t[:t.rindex("[")] if t.endswith("]") and "[" in t else t
            return [_canon_arg(x, base) for x in v]
        return v

    def fold_events(raw):
        """decode_avm_logs canonicalises addresses to 32-byte hex; re-fold those
        to registry symbols, and normalise bytesN, so both legs compare like
        for like."""
        got = decode_avm_logs(raw, avm_events)
        if got is None:
            return None
        out = []
        for lg in got:
            ins = ev_types.get(lg["name"], [])
            args = [_canon_arg(v, i2.get("type"))
                    for v, i2 in zip(lg["args"], ins)]
            out.append({"name": lg["name"], "args": args})
        return out

    # ── replay ────────────────────────────────────────────────────────────
    results, snapshots, platform_limits = {}, {}, {}
    snapshot_at = set(meta["snapshot_at"])
    for c in calls:
        i = c["i"]
        if i % 25 == 0 and ensure_app_funded(algod, dispenser, app.app_addr):
            topups += 1
        if not c.get("skip") and i not in ext_skips:
            sig, args = c["sig"], [resolve(a) for a in c["args"]]
            is_view = mut.get(sig, "") in ("view", "pure")
            prev = ln.account
            ln.account = accts.get(c["sender"]["__addr__"], dispenser) \
                if isinstance(c["sender"]["__addr__"], int) else dispenser
            try:
                if is_view:
                    r = h.call(app, sig, *args, expect_revert=True)
                    if r.reverted:
                        results[i] = {"ok": False,
                                      "revert": str(getattr(r, "fail_message", ""))[:160]}
                    else:
                        results[i] = {"ok": True, "ret": _ret(r, meta, sig, fold),
                                      "logs": []}
                else:
                    r = h.call(app, sig, *args)
                    if getattr(r, "reverted", False):
                        reason = str(getattr(r, "fail_message", "")
                                     or getattr(r, "raw_response", ""))[:200]
                        results[i] = {"ok": False, "revert": reason}
                        if is_platform_limit(reason):
                            platform_limits[i] = reason
                    else:
                        results[i] = {"ok": True, "ret": _ret(r, meta, sig, fold),
                                      "logs": fold_events(r.raw_response) or []}
            except Exception as e:
                reason = str(e)[:200]
                results[i] = {"ok": False, "revert": reason}
                if is_platform_limit(reason):
                    platform_limits[i] = reason
            finally:
                ln.account = prev
        if i in snapshot_at:
            snap = {}
            for g in meta["getters"]:
                try:
                    r = h.call(app, g["sig"], expect_revert=True)
                    if r.reverted:
                        snap[g["sig"]] = f"REVERT:{str(getattr(r,'fail_message',''))[:60]}"
                    else:
                        vs = r.abi_return
                        vs = list(vs) if len(g["outputs"]) > 1 else [vs]
                        snap[g["sig"]] = [
                            canon_value(v, o["type"], fold, o.get("components"))
                            for v, o in zip(vs, g["outputs"])]
                except Exception as e:
                    snap[g["sig"]] = f"ERROR:{str(e)[:60]}"
            snapshots[str(i)] = snap

    arc56 = {}
    for cand in sorted((case_dir / "out_avm").glob(f"{case['name']}.arc56.json")):
        arc56 = load_json(cand)
    syms = {symbol("C"): encoding.decode_address(dispenser.address),
            symbol("Z"): bytes(32)}
    for _i, _a in accts.items():
        syms[symbol(_i)] = encoding.decode_address(_a.address)
    for _ad, _i in reg["args"].items():
        syms[symbol(_i)] = bytes(12) + arg_content20(_i)
    # bytes32 keys this window's calls actually used (OZ AccessControl role
    # hashes). Derived from calls.json exactly as the EVM leg does, so the two
    # candidate sets — and hence the compared key names — are identical.
    for c in calls:
        for a in c.get("args") or []:
            if isinstance(a, dict) and set(a) == {"__b__"} and len(a["__b__"]) == 64:
                syms["0x" + a["__b__"]] = bytes.fromhex(a["__b__"])
    if evm_layout:
        # --evm-storage-layout: storage IS solc's slot layout in boxes — read
        # it exactly the way the EVM leg reads py-evm state (chd_slot_reader).
        from chd_slot_reader import read_slot_map, read_slot_storage
        slot_layout = load_json(case_dir / "storage_layout.json")
        storage = read_slot_storage(
            read_slot_map(algod, app.app_id), slot_layout, syms, fold, calls)
    else:
        storage = read_avm_storage(algod, app.app_id, arc56, fold)
        storage["maps"] = read_avm_maps(algod, app.app_id, arc56, syms, fold)
    dump_json(case_dir / "avm_results.json",
              {"results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "storage": storage,
               "platform_limits": {str(k): v for k, v in platform_limits.items()},
               "app_id": app.app_id})
    n = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[avm] replayed {n} txns ({n_ok} ok, {n-n_ok} reverted, "
          f"{len(platform_limits)} platform-limit, {topups} app top-up(s))")


def _ctype(inp):
    t = inp["type"]
    if t.startswith("tuple"):
        return "(" + ",".join(_ctype(c) for c in inp.get("components", [])) + ")" \
               + t[len("tuple"):]
    return t


def _ret(r, meta, sig, fold):
    outs = meta["fns"].get(sig, {}).get("outputs", [])
    if not outs:
        return []
    v = r.abi_return
    vs = list(v) if len(outs) > 1 else [v]
    return [canon_value(x, o["type"], fold, o.get("components"))
            for x, o in zip(vs, outs)]


if __name__ == "__main__":
    main()
