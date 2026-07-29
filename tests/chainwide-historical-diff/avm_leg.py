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
import json
import sys
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
    sp = algod.suggested_params()
    last = None
    for i, a in accts.items():
        if i not in used:
            continue
        txn = PaymentTxn(dispenser.address, sp, a.address, 5_000_000)
        last = algod.send_transaction(txn.sign(dispenser.private_key))
    if last:
        wait_for_confirmation(algod, last, 6)
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
    artifacts = h.compile(case_dir / "prepared.sol")
    app = h.deploy(artifacts, case["name"],
                   ctor_args=[resolve(m) for m in meta["ctor_args"]] or None)
    print(f"[avm] deployed {case['name']} app_id={app.app_id}")

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

    def fold_events(raw):
        """decode_avm_logs canonicalises addresses to 32-byte hex; re-fold those
        to registry symbols so both legs compare symbol-to-symbol."""
        got = decode_avm_logs(raw, avm_events)
        if got is None:
            return None
        out = []
        for lg in got:
            ins = ev_types.get(lg["name"], [])
            args = []
            for v, i2 in zip(lg["args"], ins):
                args.append(fold(v) if i2.get("type") == "address" else v)
            out.append({"name": lg["name"], "args": args})
        return out

    # ── replay ────────────────────────────────────────────────────────────
    results, snapshots, platform_limits = {}, {}, {}
    snapshot_at = set(meta["snapshot_at"])
    for c in calls:
        i = c["i"]
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

    dump_json(case_dir / "avm_results.json",
              {"results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "platform_limits": {str(k): v for k, v in platform_limits.items()},
               "app_id": app.app_id})
    n = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[avm] replayed {n} txns ({n_ok} ok, {n-n_ok} reverted, "
          f"{len(platform_limits)} platform-limit)")


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
