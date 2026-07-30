#!/usr/bin/env python3
"""Diff the two legs' replay results. Pure data — no chain access.

  python3 differ.py <case_dir>   → prints summary, writes report.json
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import dump_json, load_json

# Divergences that are DOCUMENTED EVM-vs-AVM differences, not miscompiles.
KNOWN_NOISE_GETTERS = {
    "DOMAIN_SEPARATOR()",          # EIP-712 hash over chainid + address(this)
    "getChainId()", "chainId()",
}
_NOISE_SIG_RE = re.compile(r"(DOMAIN_SEPARATOR|chainid|CHAIN_ID)", re.I)


def _dynamic(t: str) -> bool:
    return t in ("string", "bytes") or t.endswith("]")


def diff_case(case_dir: Path) -> dict:
    case = load_json(case_dir / "case.json")
    cj = load_json(case_dir / "calls.json")
    evm = load_json(case_dir / "evm_results.json")
    avm = load_json(case_dir / "avm_results.json")
    meta, calls = cj["meta"], cj["calls"]

    ev_dyn = {e["name"]: any(_dynamic(i["type"]) for i in e.get("inputs", []))
              or not e.get("inputs")
              for e in case["abi"] if e.get("type") == "event"}

    er, ar = evm["results"], avm["results"]
    findings = {"status_div": [], "value_div": [], "event_div": [],
                "event_noise": [], "snapshot_div": [], "snapshot_noise": [],
                "storage_noise": []}

    by_i = {c["i"]: c for c in calls}
    for k in sorted(set(er) | set(ar), key=int):
        e, a = er.get(k), ar.get(k)
        c = by_i.get(int(k), {})
        where = {"i": int(k), "sig": c.get("sig"), "hash": c.get("hash")}
        if e is None or a is None:
            findings["status_div"].append({**where,
                                           "detail": f"ran on only one leg "
                                                     f"(evm={e is not None}, avm={a is not None})"})
            continue
        if e["ok"] != a["ok"]:
            findings["status_div"].append({
                **where, "evm": "ok" if e["ok"] else f"REVERT {e.get('revert','')[:90]}",
                "avm": "ok" if a["ok"] else f"REVERT {a.get('revert','')[:90]}"})
            continue
        if not e["ok"]:
            continue                                    # both reverted — match
        if e.get("ret") != a.get("ret"):
            findings["value_div"].append({**where, "evm": e.get("ret"),
                                          "avm": a.get("ret")})
        el, al = e.get("logs") or [], a.get("logs") or []
        if el != al:
            # AVM-side decode gaps: zero-arg events aren't ARC-56 registered
            # (puyabug #11) and dynamic-arg events aren't tuple-decodable.
            missing = [x["name"] for x in el if x not in al]
            bucket = ("event_noise" if missing and all(ev_dyn.get(n, False) for n in missing)
                      else "event_div")
            findings[bucket].append({**where, "evm": el[:4], "avm": al[:4]})

    for k in sorted(set(evm["snapshots"]) | set(avm["snapshots"]), key=int):
        es, as_ = evm["snapshots"].get(k, {}), avm["snapshots"].get(k, {})
        for sig in sorted(set(es) | set(as_)):
            ev_, av_ = es.get(sig), as_.get(sig)
            if ev_ == av_:
                continue
            bucket = ("snapshot_noise"
                      if sig in KNOWN_NOISE_GETTERS or _NOISE_SIG_RE.search(sig)
                      else "snapshot_div")
            findings[bucket].append({"after_txn": int(k), "getter": sig,
                                     "evm": ev_, "avm": av_})

    # ── storage diffing (by Solidity variable NAME, across two storage models) ──
    es, as_ = evm.get("storage") or {}, avm.get("storage") or {}
    findings["storage_div"], findings["storage_map_div"] = [], []
    delta = evm.get("storage_delta") or {}

    def _last_change(var):
        """Localise: the last txn whose EVM delta touched this variable."""
        hits = [int(i) for i, d in delta.items() if var in d]
        return max(hits) if hits else None

    e_sc, a_sc = es.get("scalars") or {}, as_.get("scalars") or {}
    for var in sorted(set(e_sc) | set(a_sc)):
        if var.startswith("__"):
            continue
        ev_, av_ = e_sc.get(var), a_sc.get(var)
        if ev_ == av_:
            continue
        # A var only the EVM side reports is usually a puya-sol representation
        # choice (e.g. immutables/constants not materialised as app state), not
        # a value divergence — flag separately from a genuine value mismatch.
        bucket = "storage_div" if (var in e_sc and var in a_sc) else "storage_noise"
        findings.setdefault(bucket, []).append(
            {"var": var, "evm": ev_, "avm": av_, "last_changed_txn": _last_change(var)})

    e_m, a_m = es.get("maps") or {}, as_.get("maps") or {}
    declared = set(a_m.pop("__declared__", []) or [])
    # COVERAGE, not correctness: a mapping the contract declares but that the EVM
    # side never read is compared against NOTHING, which would otherwise be
    # indistinguishable from "clean". op_gov/_balances and opmint9/_balances were
    # silently skipped this way (namespaced ERC-7201 storage / non-address keys /
    # array-or-struct values, which the EVM storageLayout walk doesn't cover).
    uncompared = sorted(declared - (set(e_m) & set(a_m)))
    if uncompared:
        findings["storage_maps_uncompared"] = [
            {"maps": uncompared,
             "note": "declared by the contract but NOT diffed — no coverage here"}]
    # Guard against a VACUOUS pass: if the EVM side found mapping state but the
    # AVM side reported none, the comparison did not happen — surface that
    # explicitly instead of silently counting zero divergences.
    if e_m and not {k for k in a_m if not k.startswith("__")}:
        findings["storage_maps_unavailable"] = [
            {"evm_maps": sorted(e_m), "avm_maps": sorted(a_m),
             "note": "AVM box enumeration returned nothing — mapping storage NOT compared"}]
    for m in sorted(set(e_m) & set(a_m)):
        if m.startswith("__"):
            continue
        ee, aa = e_m.get(m) or {}, a_m.get(m) or {}
        # Both sides are keyed by registry SYMBOL (the AVM leg computes box
        # names forward through puya-sol's hash), so entries compare 1:1.
        for k in sorted(set(ee) | set(aa)):
            if ee.get(k) != aa.get(k):
                findings["storage_map_div"].append(
                    {"map": m, "key": k, "evm": ee.get(k), "avm": aa.get(k)})

    skips = {}
    for c in calls:
        if c.get("skip"):
            skips[c["skip"].split(":")[0]] = skips.get(c["skip"].split(":")[0], 0) + 1
    report = {
        "tag": case["tag"], "name": case["name"], "address": case["address"],
        "txns_in_window": len(calls),
        "replayed": len(er),
        "skips": skips,
        "platform_limits": len(avm.get("platform_limits") or {}),
        "findings": findings,
        "counts": {k: len(v) for k, v in findings.items()},
    }
    dump_json(case_dir / "report.json", report)
    return report


def print_report(rep: dict):
    c = rep["counts"]
    real = sum(c.get(k, 0) for k in ("status_div", "value_div", "event_div",
                                     "snapshot_div", "storage_div", "storage_map_div"))
    print(f"\n=== {rep['tag']} ({rep['name']}) — {rep['replayed']}/{rep['txns_in_window']} "
          f"txns replayed on both legs ===")
    print(f"  skips: {rep['skips'] or '{}'}  | avm platform-limits: {rep['platform_limits']}")
    for k in ("status_div", "value_div", "event_div", "snapshot_div",
              "storage_div", "storage_map_div"):
        if c[k]:
            print(f"  ❌ {k}: {c[k]}")
            for f in rep["findings"][k][:5]:
                print(f"       {f}")
    for k in ("event_noise", "snapshot_noise", "storage_noise"):
        pass
    for k in ("storage_maps_unavailable", "storage_maps_uncompared"):
        if c.get(k):
            det = rep["findings"][k][0]
            print(f"  ⚠️  {k}: {det.get('maps', 'mapping storage')} not diffed (see report)")
    for k in ("event_noise", "snapshot_noise", "storage_noise"):
        if c[k]:
            print(f"  · {k} (known EVM/AVM difference): {c[k]}")
    print("  ✅ no divergences" if real == 0 else f"  ❌ {real} REAL divergence(s)")


if __name__ == "__main__":
    print_report(diff_case(Path(sys.argv[1]).resolve()))
