#!/usr/bin/env python3
"""Diff the two legs' replay results. Pure data — no chain access.

  python3 differ.py <case_dir>   → prints summary, writes report.json
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chd_common import dump_json, is_platform_limit, load_json, replay_epoch

# Divergences that are DOCUMENTED EVM-vs-AVM differences, not miscompiles.
KNOWN_NOISE_GETTERS = {
    "DOMAIN_SEPARATOR()",          # EIP-712 hash over chainid + address(this)
    "getChainId()", "chainId()",
    "clock()",                     # ERC-6372: block.number — EVM leg's local
                                   # height vs the AVM round can never match
}
_NOISE_SIG_RE = re.compile(r"(DOMAIN_?SEPARATOR|chainid|CHAIN_ID)", re.I)

# Matches replay.py's own drift warning. Measured, not taste: DEGEN replays
# clean at +938 d and fails near +1500 d, so a year of shift is where a red
# report stops being trustworthy on its own.
_EPOCH_SHIFT_MAX = 365 * 24 * 3600

# The buckets that mean "a real difference was observed".
_REAL_BUCKETS = ("status_div", "value_div", "event_div", "snapshot_div",
                 "probe_div", "storage_div", "storage_map_div",
                 "storage_raw_div")

_HEX_RE = re.compile(r"^\??0x[0-9a-fA-F]+$")


def _hex_norm(v):
    """Numeric view of hex-string observables (recursively for lists):
    '?0x00…5e7ec' and '?0x…0005e7ec' are the same value at different widths."""
    if isinstance(v, list):
        return [_hex_norm(x) for x in v]
    if isinstance(v, dict):
        return {key: _hex_norm(value) for key, value in v.items()}
    if isinstance(v, str) and _HEX_RE.match(v):
        return int(v.lstrip("?"), 16)
    return v


# How far apart two legs' clocks can plausibly be. Both legs now drive their
# block time to the same replayed instants (avm_leg.BlockClock / evm_leg's
# time_travel), so this covers only the residual: py-evm advances its own clock
# a second per mined block, which drifts against the AVM whenever several calls
# share one historical second. Measured across permit2's window the legs agreed
# to [-5, +1] s; the margin here is for denser windows.
#
# It was 7 DAYS before pinning, when the AVM leg ran at LocalNet wall clock and
# the EVM leg at historical time — wide enough to absorb a genuinely wrong
# timestamp. Keep it tight: a false positive here is visible and diagnosable, a
# false negative is not.
_TS_SKEW_MAX = 300


def _timestamp_noise_elems(ev_, av_):
    """Element-wise twin of `_timestamp_noise` for STRUCT/ARRAY map values.

    A struct field set from `block.timestamp` (Permit2's PackedAllowance
    `expiration`, which `_updateApproval` fills with `now` when the caller
    passes 0) can still differ by the residual per-block drift described at
    `_TS_SKEW_MAX`. Two lists that match except at positions holding plausible
    unix timestamps within that drift are skew, not a miscompile."""
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
    """Deploy-time clock skew: a ctor storing block.timestamp lands a little
    apart on the two legs (the EVM leg runs, then the AVM leg deploys). Two
    plausible-unix-timestamp values within `_TS_SKEW_MAX` of each other are the
    run itself, not a compilation divergence (ena/lastMintTimestamp,
    cow/timestampLastMinting, pol/lastMint)."""
    def flat(v):
        if isinstance(v, list) and len(v) == 1:
            return v[0]
        return v
    a, b = flat(ev_), flat(av_)
    return (isinstance(a, int) and isinstance(b, int)
            and a > 1_500_000_000 and b > 1_500_000_000
            and abs(a - b) < _TS_SKEW_MAX)


def _is_default(value) -> bool:
    """True when a decoded value is entirely the Solidity type default."""
    if value is None or value is False or value == 0 or value == "":
        return True
    if isinstance(value, str):
        return value.startswith("0x") and not any(
            char != "0" for char in value[2:])
    if isinstance(value, (list, tuple)):
        return all(_is_default(item) for item in value)
    if isinstance(value, dict):
        return all(_is_default(item) for item in value.values())
    return False


def _height_skew_noise(ev_, av_, wtxn, evm_no, avm_no):
    """Block-HEIGHT skew: a contract storing `block.number` (staup's `_locked`
    = block.number + lockPeriod) writes each leg's own local chain height —
    py-evm restarts fresh per run while LocalNet's round count accumulates
    across the whole session, so the pair can never match and the gap varies
    by session. Absorb ONLY when the numeric delta equals the two legs'
    RECORDED height skew at the writing txn (±2 for the lands-next-round
    convention), and the values are height-sized — a tiny counter can never be
    absorbed, so a real off-by-one stays a finding.
    """
    if wtxn is None or evm_no is None or avm_no is None:
        return False
    # `last_write_txn` is MAP-level; an individual key may have been written a
    # few txns earlier, when the skew was smaller (the AVM gains ~2 rounds per
    # call — seal + call — against the EVM's 1, so the gap drifts per txn).
    # Accept a delta that equals the skew at ANY txn up to the map's last
    # write: the pair must still coincide with a historically OBSERVED skew
    # value, so an arbitrary wrong number is still a finding.
    skews = []
    for k, e_no in evm_no.items():
        a_no = avm_no.get(k)
        if a_no is None:
            continue
        try:
            if int(k) <= int(wtxn) + 1:
                skews.append(int(a_no) - int(e_no))
        except (TypeError, ValueError):
            continue
    if not skews:
        return False

    def _pair_ok(e, a):
        if e == a:
            return True
        return (isinstance(e, int) and isinstance(a, int)
                and not isinstance(e, bool) and not isinstance(a, bool)
                and min(e, a) > 1000
                and any(abs((a - e) - sk) <= 2 for sk in skews))

    def flat(v):
        return v[0] if isinstance(v, list) and len(v) == 1 else v
    e, a = flat(ev_), flat(av_)
    if isinstance(e, list) and isinstance(a, list) and len(e) == len(a) and e:
        saw = False
        for x, y in zip(e, a):
            if x == y:
                continue
            if _pair_ok(x, y):
                saw = True
                continue
            return False
        return saw
    return e != a and _pair_ok(e, a)


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
    evm_block_no = evm.get("block_no") or {}
    avm_block_no = avm.get("block_no") or {}
    findings = {"status_div": [], "value_div": [], "event_div": [],
                "event_noise": [], "snapshot_div": [], "snapshot_noise": [],
                "probe_div": [], "probe_noise": [],
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
            # An AVM-side PLATFORM LIMIT (opcode/box/group budget) is a
            # documented constraint, never a miscompile. The orchestrator
            # normally re-skips these symmetrically, but its convergence loop is
            # capped at 3 attempts and each pass can uncover new ones — the
            # stragglers were landing in status_div, exactly the masquerade the
            # platform-limit class exists to prevent.
            bucket = ("platform_limit_noise"
                      if not a["ok"] and is_platform_limit(a.get("revert", ""))
                      else "status_div")
            findings.setdefault(bucket, []).append({
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
        # A snapshot present on ONE leg only is probe absence, not state
        # divergence — comparing value against nothing manufactured 18 fake
        # divergences at FLOKI's txn 399. The legs now snapshot symmetrically;
        # this guard keeps any residual asymmetry honest (counted, not REAL).
        if not es or not as_:
            findings["snapshot_noise"].append(
                {"after_txn": int(k), "getter": "*",
                 "note": f"snapshot-absent on {'evm' if not es else 'avm'} leg"})
            continue
        for sig in sorted(set(es) | set(as_)):
            ev_, av_ = es.get(sig), as_.get(sig)
            if ev_ == av_:
                continue
            # Width-insensitive hex equality: an address/bytesN constant can
            # surface at different byte widths per leg (pol's PERMIT2() —
            # 20-byte EVM word vs the AVM's wider zero-padded form). Values
            # that parse to the SAME integer are the same observable.
            if _hex_norm(ev_) == _hex_norm(av_):
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

    probes = meta.get("probes") or []
    ep, ap = evm.get("probes") or {}, avm.get("probes") or {}
    # Report the CLOCK, not just its consequences: probes answered at two
    # different instants make every accruing view differ by a small, uniform,
    # entirely plausible-looking amount. Aave's eight assets all drifted by
    # 1.48e-7 of the window that way, which reads as a ray-math miscompile.
    e_probe_ts, a_probe_ts = evm.get("probe_time"), avm.get("probe_time")
    if e_probe_ts and a_probe_ts and int(e_probe_ts) != int(a_probe_ts):
        findings["probe_clock_skew"] = [
            {"evm": int(e_probe_ts), "avm": int(a_probe_ts),
             "delta": int(a_probe_ts) - int(e_probe_ts),
             "note": "probe phase evaluated at different instants — accruing "
                     "views differ for that reason alone"}]
    for key in sorted(set(ep) | set(ap), key=int):
        ev_, av_ = ep.get(key), ap.get(key)
        spec = probes[int(key)] if int(key) < len(probes) else {}
        where = {"probe": int(key), "getter": spec.get("sig"),
                 "args": spec.get("args")}
        if ev_ is None or av_ is None:
            findings["probe_noise"].append({
                **where, "note": "probe absent on one leg"})
        elif ev_.get("ok") != av_.get("ok"):
            findings["probe_div"].append({**where, "evm": ev_, "avm": av_})
        elif not ev_.get("ok"):
            # Revert text is VM-specific; matching status is the observable.
            continue
        elif ev_.get("ret") != av_.get("ret") \
                and _hex_norm(ev_.get("ret")) != _hex_norm(av_.get("ret")):
            findings["probe_div"].append({**where, "evm": ev_.get("ret"),
                                           "avm": av_.get("ret")})

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
        # Width-insensitive hex + list-recursive numeric view ("0x…01" vs 1,
        # [len] vs ['0x…01']) — mapping-carrying-struct summaries compare by
        # value, not rendering.
        if _hex_norm(x) == _hex_norm(y):
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
        # An EIP-712 domain separator CACHED IN STORAGE is the same chain-id
        # noise the getter rule already covers — it hashes chainid in, so the
        # two legs must differ. Only the getter spelling was classified, so a
        # contract that caches it in a state var (wallettok) reported it as a
        # real divergence.
        if _NOISE_SIG_RE.search(var):
            bucket = "storage_noise"
        if _timestamp_noise(ev_, av_):
            bucket = "storage_noise"
        if _height_skew_noise(ev_, av_, _last_change(var),
                              evm_block_no, avm_block_no):
            bucket = "storage_noise"
        findings.setdefault(bucket, []).append(
            {"var": var, "evm": ev_, "avm": av_, "last_changed_txn": _last_change(var)})

    e_m, a_m = es.get("maps") or {}, as_.get("maps") or {}
    declared = set(a_m.pop("__declared__", []) or [])
    stray_boxes = a_m.pop("__unattributed_boxes__", 0) or 0
    stray_groups = a_m.pop("__unattributed_box_groups__", {}) or {}
    unsupported = a_m.pop("__unsupported__", []) or []
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
    if unsupported:
        findings.setdefault("storage_maps_uncompared", []).append(
            {"maps": unsupported,
             "note": "ARC-56 mapping has no corresponding solc layout root"})
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
             "groups": stray_groups,
             "note": "boxes on chain that no derived mapping key matched — "
                     "either a shape the reader skips or a wrong key derivation"}]
    writes = es.get("writes") or {}
    e_raw, a_raw = es.get("raw_slots") or {}, as_.get("raw_slots") or {}
    raw_compared = set(e_raw) & set(a_raw)
    for slot in sorted(raw_compared, key=int):
        if not _same_word(e_raw[slot], a_raw[slot]):
            findings.setdefault("storage_raw_div", []).append(
                {"slot": slot, "evm": e_raw[slot], "avm": a_raw[slot]})
    raw_uncompared = sorted(set(a_raw) - set(e_raw), key=int)
    if raw_uncompared:
        findings["storage_raw_uncompared"] = [{
            "slots": raw_uncompared,
            "note": "native s:<slot> boxes whose EVM word was not captured"}]
    blind_slots = es.get("blind_slots") or {}
    residual_blind = {slot: txns for slot, txns in blind_slots.items()
                      if slot not in raw_compared}
    residual_blind_count = max(
        len(residual_blind),
        int(es.get("blind_slot_count") or 0) - len(raw_compared))
    if residual_blind_count:
        findings["storage_blind_slots"] = [
            {"slots": residual_blind_count,
             "groups": es.get("blind_slot_groups") or {},
             "sample": list(residual_blind.items())[:12],
             "note": "written by the contract but read by NO differ probe — "
                     "not compared on either leg"}]

    def _last_write(mapname):
        """Localise a map divergence to the last txn that wrote that map."""
        hits = [int(i) for i, w in writes.items() if mapname in (w.get("names") or ())]
        return max(hits) if hits else None

    for m in sorted(set(e_m) & set(a_m)):
        if m.startswith("__"):
            continue
        ee, aa = e_m.get(m) or {}, a_m.get(m) or {}
        # Both sides are keyed by registry SYMBOL (the AVM leg computes box
        # names forward through puya-sol's hash), so entries compare 1:1.
        keys = [k for k in sorted(set(ee) | set(aa))
                if not _same_word(ee.get(k), aa.get(k))]
        for k in keys:
            ev_k, av_k = ee.get(k), aa.get(k)
            height_only = _height_skew_noise(
                ev_k, av_k, _last_write(m), evm_block_no, avm_block_no)
            ts_only = (_timestamp_noise(ev_k, av_k)
                       or _timestamp_noise_elems(ev_k, av_k))
            # An entry holding the type default is indistinguishable from an
            # absent one on the EVM leg, which reads slots and cannot enumerate
            # a mapping. A native box storing that default is therefore the
            # same state, not an extra entry — only a NON-default value present
            # on one leg alone is a real difference.
            default_only = ((k not in ee or k not in aa)
                            and _is_default(av_k if k not in ee else ev_k))
            bucket = ("storage_noise"
                      if (height_only or ts_only or default_only)
                      else "storage_map_div")
            f = {"map": m, "key": k, "evm": ev_k, "avm": av_k,
                 "last_write_txn": _last_write(m)}
            if default_only:
                f["note"] = ("present on one leg holding the type default — "
                             "the other leg cannot distinguish that from absent")
            elif height_only:
                f["note"] = "differs only by the recorded local block-height skew"
            elif ts_only:
                f["note"] = ("differs only in plausible-timestamp field(s) "
                             "within the two legs' residual clock skew")
            findings.setdefault(bucket, []).append(f)

    skips = {}
    for c in calls:
        if c.get("skip"):
            skips[c["skip"].split(":")[0]] = skips.get(c["skip"].split(":")[0], 0) + 1
    # A replay far from its own historical window is an ENVIRONMENT caveat:
    # LocalNet's clock is a ratchet and py-evm starts at wall clock, so an old
    # window always runs shifted and time-gated code can fail on one leg alone
    # (bgb: 192 of 200 txns reverting, reported as 342 divergences). Attach it
    # ONLY to a red report — the shift is unremarkable on its own, and a
    # permanent warning on every green case would train the reader to skip it.
    _shift = int(evm.get("time_base") or 0) - replay_epoch(calls)
    if _shift > _EPOCH_SHIFT_MAX and any(
            findings.get(k) for k in _REAL_BUCKETS):
        findings["clock_epoch_shift"] = [{
            "shift_days": _shift // 86400,
            "note": "replayed far from its own historical window — time-gated "
                    "code can fail on the AVM leg alone, so these divergences "
                    "may be environmental; `algokit localnet reset` rewinds "
                    "the chain clock so the window replays at true time"}]

    report = {
        "tag": case["tag"], "name": case["name"], "address": case["address"],
        "txns_in_window": len(calls),
        "replayed": len(er),
        "skips": skips,
        "platform_limits": len(avm.get("platform_limits") or {}),
        "findings": findings,
        "counts": {k: len(v) for k, v in findings.items()},
        "coverage": {
            "parameterized_probes": {
                "planned": len(probes),
                "compared": len(set(ep) & set(ap)),
                "evm_successes": sum(bool(item.get("ok")) for item in ep.values()),
                "avm_successes": sum(bool(item.get("ok")) for item in ap.values()),
            },
            "mapping_roots_declared": sorted(declared),
            "mapping_roots_compared": sorted(
                declared & set(e_m) & set(a_m)),
            "raw_slots_compared": len(raw_compared),
            "evm": es.get("coverage") or {},
            "avm": as_.get("coverage") or {},
        },
    }
    dump_json(case_dir / "report.json", report)
    return report


def print_report(rep: dict):
    c = rep["counts"]
    real = sum(c.get(k, 0) for k in _REAL_BUCKETS)
    print(f"\n=== {rep['tag']} ({rep['name']}) — {rep['replayed']}/{rep['txns_in_window']} "
          f"txns replayed on both legs ===")
    print(f"  skips: {rep['skips'] or '{}'}  | avm platform-limits: {rep['platform_limits']}")
    # COVERAGE, loudly. A run whose closed-world filter dropped most of the
    # window prints the same ✅ as a full one: cctp_messenger replayed 20 of 300
    # txns and read as clean, and the 280 skips were a dep that could not route
    # a call at all. Surface the ratio next to the verdict, not in the JSON.
    _win = int(rep.get("txns_in_window") or 0)
    _ran = int(rep.get("replayed") or 0)
    if _win and _ran / _win < 0.5:
        print(f"  ⚠️  only {_ran}/{_win} txns replayed ({100 * _ran / _win:.0f}%) — "
              f"a verdict over this little of the window is close to vacuous")
    probes = (rep.get("coverage") or {}).get("parameterized_probes") or {}
    if probes.get("planned"):
        print(f"  parameterized probes: {probes.get('compared', 0)}/"
              f"{probes['planned']} compared")
    for k in _REAL_BUCKETS:
        if c.get(k):
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
    if c.get("storage_raw_uncompared"):
        det = rep["findings"]["storage_raw_uncompared"][0]
        print(f"  ⚠️  storage_raw_uncompared: {len(det['slots'])} raw slot(s) "
              f"not captured on EVM")
    for k in ("event_noise", "snapshot_noise", "probe_noise", "storage_noise"):
        if c.get(k):
            print(f"  · {k} (known EVM/AVM difference): {c[k]}")
    if c.get("clock_epoch_shift"):
        det = rep["findings"]["clock_epoch_shift"][0]
        print(f"  ⚠️  replayed +{det['shift_days']}d from its historical window "
              f"— divergences may be environmental (`algokit localnet reset`)")
    print("  ✅ no divergences" if real == 0 else f"  ❌ {real} REAL divergence(s)")


if __name__ == "__main__":
    print_report(diff_case(Path(sys.argv[1]).resolve()))
