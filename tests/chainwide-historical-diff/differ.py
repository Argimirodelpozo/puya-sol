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
    "clock()",                     # ERC-6372: block.number — EVM leg's local
                                   # height vs the AVM round can never match
}
_NOISE_SIG_RE = re.compile(r"(DOMAIN_SEPARATOR|chainid|CHAIN_ID)", re.I)


# How far apart two legs' clocks can plausibly be. Not just run-to-run skew:
# LocalNet's block time advances with BLOCK PRODUCTION, so an idle chain's
# `LatestTimestamp` trails wall clock by however long it sat unused — measured
# at 2.5 h on a working session, and unbounded in principle. Both values must
# still be plausible unix timestamps, so a genuinely wrong field (0, a small
# counter, a hash) is never absorbed. The real fix is pinning AVM block time
# via algod dev-mode, which would also convert some closed-world skips into
# coverage.
_TS_SKEW_MAX = 7 * 24 * 3600


def _timestamp_noise_elems(ev_, av_):
    """Element-wise twin of `_timestamp_noise` for STRUCT/ARRAY map values.

    A struct field set from `block.timestamp` (Permit2's PackedAllowance
    `expiration`, which `_updateApproval` fills with `now` when the caller
    passes 0) differs between legs because the EVM leg time-travels to each
    txn's historical timestamp while the AVM leg runs at LocalNet wall clock.
    Two lists that match except at positions holding plausible unix
    timestamps within 2h of each other are that skew, not a miscompile."""
    if not (isinstance(ev_, list) and isinstance(av_, list)
            and len(ev_) == len(av_) and ev_):
        return False
    saw_ts = False
    for a, b in zip(ev_, av_):
        if a == b:
            continue
        if (isinstance(a, int) and isinstance(b, int)
                and a > 1_500_000_000 and b > 1_500_000_000
                and abs(a - b) < _TS_SKEW_MAX):
            saw_ts = True
            continue
        return False
    return saw_ts


def _timestamp_noise(ev_, av_):
    """Deploy-time wall-clock skew: a ctor storing block.timestamp lands a few
    minutes apart on the two legs (EVM leg runs, then the AVM leg deploys).
    Two plausible-unix-timestamp values within 2h of each other are the run
    itself, not a compilation divergence (ena/lastMintTimestamp)."""
    def flat(v):
        if isinstance(v, list) and len(v) == 1:
            return v[0]
        return v
    a, b = flat(ev_), flat(av_)
    return (isinstance(a, int) and isinstance(b, int)
            and a > 1_500_000_000 and b > 1_500_000_000
            and abs(a - b) < _TS_SKEW_MAX)


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
            # EIP-5267 eip712Domain(): field 3 is the CHAIN ID — pure
            # environment (py-evm's id vs the AVM's fixed 1), not compilation.
            # Compare the other six fields for real; matching = noise.
            if (sig == "eip712Domain()" and isinstance(ev_, list)
                    and isinstance(av_, list) and len(ev_) == len(av_)
                    and len(ev_) >= 4
                    and [x for i2, x in enumerate(ev_) if i2 != 3]
                        == [x for i2, x in enumerate(av_) if i2 != 3]):
                findings["snapshot_noise"].append(
                    {"after_txn": int(k), "getter": sig,
                     "evm": ev_, "avm": av_, "note": "chainid-only"})
                continue
            # BOTH legs reverting is agreement on the observable outcome. The
            # messages are not comparable across VMs — the EVM leg carries the
            # ABI revert payload, the AVM leg an algod transaction id — so
            # comparing the strings manufactures a divergence for every
            # legitimately-reverting getter (staup's getLockPeriod, an
            # owner-gated view, tripped it on all 16 snapshots). Revert PAYLOAD
            # comparison for real calls is handled by the value/status differ,
            # which is where it belongs.
            both_revert = (isinstance(ev_, str) and isinstance(av_, str)
                           and ev_.startswith("REVERT:") and av_.startswith("REVERT:"))
            bucket = ("snapshot_noise"
                      if both_revert or sig in KNOWN_NOISE_GETTERS
                      or _NOISE_SIG_RE.search(sig)
                      or _timestamp_noise(ev_, av_)
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

    def _same_word(x, y):
        """A 32-byte slot rendered as "0x…" hex on one leg and as an int on the
        other is the SAME 256-bit word.

        The EVM leg knows a var is `bytes32` from solc's storageLayout and emits
        hex; the AVM leg cannot, because puya-sol declares it in arc56 as the
        untyped `AVMBytes` (xerc20/_PERMIT_TYPEHASH_DEPRECATED_SLOT read
        "0x000…0" vs 0 — the same zero, reported as a divergence). Comparing
        numerically is lossless, so a genuinely different value still differs.
        """
        if x == y:
            return True
        for a, b in ((x, y), (y, x)):
            if isinstance(a, str) and a.startswith("0x") and isinstance(b, int) \
                    and not isinstance(b, bool):
                try:
                    return int(a, 16) == b
                except ValueError:
                    return False
        return False

    e_sc, a_sc = es.get("scalars") or {}, as_.get("scalars") or {}
    for var in sorted(set(e_sc) | set(a_sc)):
        if var.startswith("__"):
            continue
        ev_, av_ = e_sc.get(var), a_sc.get(var)
        if _same_word(ev_, av_):
            continue
        # A var only the EVM side reports is usually a puya-sol representation
        # choice (e.g. immutables/constants not materialised as app state), not
        # a value divergence — flag separately from a genuine value mismatch.
        bucket = "storage_div" if (var in e_sc and var in a_sc) else "storage_noise"
        if _timestamp_noise(ev_, av_):
            bucket = "storage_noise"
        findings.setdefault(bucket, []).append(
            {"var": var, "evm": ev_, "avm": av_, "last_changed_txn": _last_change(var)})

    e_m, a_m = es.get("maps") or {}, as_.get("maps") or {}
    declared = set(a_m.pop("__declared__", []) or [])
    stray_boxes = a_m.pop("__unattributed_boxes__", 0) or 0
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
    # SSTORE trace: the EVM leg records every slot each txn actually wrote and
    # marks the ones no reader looked at. Those are state the differ is BLIND to
    # — a mapping with non-address keys, a nested struct, an ERC-7201 namespace.
    # Without this, "0 divergences" cannot be distinguished from "0 compared".
    # The AVM mirror of a blind slot: a box that EXISTS but that no forward-
    # derived candidate name matched. This is the only check that can catch a
    # WRONG key derivation — get the hash wrong and both legs find nothing for a
    # map, which is indistinguishable from a genuinely empty map.
    if stray_boxes:
        findings["storage_boxes_unattributed"] = [
            {"boxes": stray_boxes,
             "note": "boxes on chain that no derived mapping key matched — "
                     "either a shape the reader skips or a wrong key derivation"}]
    writes = es.get("writes") or {}
    if es.get("blind_slot_count"):
        findings["storage_blind_slots"] = [
            {"slots": es["blind_slot_count"],
             "sample": list((es.get("blind_slots") or {}).items())[:5],
             "note": "written by the contract but read by NO differ probe — "
                     "not compared on either leg"}]

    def _last_write(mapname):
        """Localise a map divergence to the last txn that wrote that map."""
        hits = [int(i) for i, w in writes.items() if mapname in (w.get("names") or ())]
        return max(hits) if hits else None

    def _uniform_offset(diffs):
        """Signature of a BLOCK/TIME base difference rather than a miscompile.

        The EVM leg pins block numbers and timestamps to historical values; the
        AVM leg cannot (LocalNet's round is whatever it is). A contract storing
        `block.number + period` therefore differs on EVERY entry by the SAME
        constant, with every other field equal. staup does exactly that
        (`_locked[a] = lockAddressInfo(block.number + blocklockperiod, true)`):
        16 entries, one delta of 487989, bool matching on all 16.

        Requires >= 2 entries and one distinct non-zero delta, so it cannot
        absorb a one-off wrong value. Still reported — as noise, not a finding.
        """
        if len(diffs) < 2:
            return None
        deltas = set()
        for ev_, av_ in diffs:
            e_l = ev_ if isinstance(ev_, list) else [ev_]
            a_l = av_ if isinstance(av_, list) else [av_]
            if len(e_l) != len(a_l):
                return None
            d = None
            for x, y in zip(e_l, a_l):
                if x == y:
                    continue
                if not (isinstance(x, int) and isinstance(y, int)) \
                        or isinstance(x, bool) or isinstance(y, bool):
                    return None          # a non-numeric field differs => real
                if d is not None:
                    return None          # two numeric fields differ => real
                d = y - x
            if d in (None, 0):
                return None
            deltas.add(d)
        return deltas.pop() if len(deltas) == 1 else None

    for m in sorted(set(e_m) & set(a_m)):
        if m.startswith("__"):
            continue
        ee, aa = e_m.get(m) or {}, a_m.get(m) or {}
        # Both sides are keyed by registry SYMBOL (the AVM leg computes box
        # names forward through puya-sol's hash), so entries compare 1:1.
        keys = [k for k in sorted(set(ee) | set(aa))
                if not _same_word(ee.get(k), aa.get(k))]
        off = _uniform_offset([(ee.get(k), aa.get(k)) for k in keys])
        for k in keys:
            ev_k, av_k = ee.get(k), aa.get(k)
            ts_only = (_timestamp_noise(ev_k, av_k)
                       or _timestamp_noise_elems(ev_k, av_k))
            bucket = ("storage_noise"
                      if (off is not None or ts_only) else "storage_map_div")
            f = {"map": m, "key": k, "evm": ev_k, "avm": av_k,
                 "last_write_txn": _last_write(m)}
            if off is not None:
                f["note"] = (f"uniform +{off} on every entry — EVM/AVM block or "
                             "timestamp base, not a value divergence")
            elif ts_only:
                f["note"] = ("differs only in plausible-timestamp field(s) — the "
                             "EVM leg time-travels to each txn's historical "
                             "timestamp, the AVM leg runs at LocalNet wall clock")
            findings.setdefault(bucket, []).append(f)

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
    for k in ("storage_maps_unavailable", "storage_maps_uncompared"):
        if c.get(k):
            det = rep["findings"][k][0]
            print(f"  ⚠️  {k}: {det.get('maps', 'mapping storage')} not diffed (see report)")
    if c.get("storage_boxes_unattributed"):
        det = rep["findings"]["storage_boxes_unattributed"][0]
        print(f"  ⚠️  storage_boxes_unattributed: {det['boxes']} box(es) matched "
              f"no derived mapping key")
    if c.get("storage_blind_slots"):
        det = rep["findings"]["storage_blind_slots"][0]
        print(f"  ⚠️  storage_blind_slots: {det['slots']} slot(s) written but "
              f"never probed — not compared")
    for k in ("event_noise", "snapshot_noise", "storage_noise"):
        if c[k]:
            print(f"  · {k} (known EVM/AVM difference): {c[k]}")
    print("  ✅ no divergences" if real == 0 else f"  ❌ {real} REAL divergence(s)")


if __name__ == "__main__":
    print_report(diff_case(Path(sys.argv[1]).resolve()))
