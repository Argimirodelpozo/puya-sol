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
import urllib.request
from pathlib import Path

from eth_abi import decode as evm_abi_decode, encode as evm_abi_encode

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))
sys.path.insert(0, str(HERE.parents[0] / "WIP" / "tiny-fuzzing-oracle"))

from chd_common import (algo_sender_seed, arg_content20,
                        build_dep_tape_plans, bytes32_mapping_key_candidates,
                        tape_script_chunks, canon_value,
                        dump_json, is_platform_limit, load_json,
                        probe_clock_target, replay_clock_targets, symbol)
from chd_storage import KeyEvidence, NativeStorageReader

from algosdk import encoding
from algosdk.transaction import PaymentTxn, wait_for_confirmation
from nacl.signing import SigningKey

from framework import Harness
from framework.call import raw_call_fee_ceiling
from framework.deploy import update_program
from framework.localnet import LocalNet
from event_diff import decode_avm_log_bytes


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


class BlockClock:
    """Drives LocalNet's block clock so this leg sees the same block.timestamp
    the EVM leg replayed at.

    Two algod dev-mode behaviours make this work; both were measured, and both
    are the opposite of the obvious guess:

      * `global LatestTimestamp` reports the PREVIOUS block's timestamp, not
        the one carrying the call — so reaching a target means sealing a block
        AT that instant and then calling.
      * once a timestamp offset has been set, dev mode stops consulting the
        real clock and computes each block as `previous + offset`. The clock
        is therefore RELATIVE, which is the only reason 2022 is reachable at
        all: the offset endpoint is uint64 and cannot express a jump backwards.

    Holding the offset at 0 between jumps is what keeps deploy, funding and
    top-up txns from drifting time underneath the replay.
    """

    def __init__(self, algod, dispenser, enabled=True):
        self.algod, self.dispenser, self.enabled = algod, dispenser, enabled
        self.cur, self.jumps = None, 0

    def _set_offset(self, n):
        # Raw urllib, not algod_request: algosdk decorates the URL and algod
        # answers 404 for the dev-mode route.
        url = f"{self.algod.algod_address.rstrip('/')}/v2/devmode/blocks/offset/{int(n)}"
        req = urllib.request.Request(
            url, method="POST", headers={"X-Algo-API-Token": self.algod.algod_token})
        urllib.request.urlopen(req, timeout=10).read()

    def _seal(self):
        sp = self.algod.suggested_params()
        txid = self.algod.send_transaction(
            PaymentTxn(self.dispenser.address, sp, self.dispenser.address, 0)
            .sign(self.dispenser.private_key))
        wait_for_confirmation(self.algod, txid, 6)

    def _read(self):
        st = self.algod.status()
        # A freshly reset devnet's round-zero block has no timestamp field.
        # Treat that genesis sentinel as time zero so the first explicit clock
        # anchor can be installed before any ordinary transaction adopts wall
        # time.
        return int(self.algod.block_info(st["last-round"])["block"].get("ts", 0))

    def advance_to(self, ts):
        """Make the next call observe exactly `ts`. Never moves time backwards
        (the historical window is in chain order, so deltas are non-negative;
        two txns in one block legitimately want the same instant)."""
        if not self.enabled or not ts:
            return
        if self.cur is None:
            self.cur = self._read()
        delta = int(ts) - self.cur
        if delta <= 0:
            return
        self._set_offset(delta)
        self._seal()
        self._set_offset(0)
        self.cur = int(ts)
        self.jumps += 1

    # NB: deliberately no restore-to-wall-clock. Jumping the chain forward at
    # the end of a case would ratchet it past the next case's window, forcing
    # every later replay onto a shifted epoch — which is what makes time-gated
    # constructors fail. The chain is left at this window's end instead, and
    # `algokit localnet reset` rewinds it to 0 when the drift stops paying.


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


def _read_avm_maps_legacy(algod, app_id, arc56, syms, fold, calls=None):
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
        # Mapping-carrying struct values (OZ EnumerableSet in a mapping): the
        # non-mapping fields live at chain++"_inner", holding the INNER
        # struct's ARC4 encoding. Summarise dynamic-array members to their
        # LENGTH — that is exactly what the EVM leg reads at the struct's
        # member slot (the array's length word) — so entries compare 1:1.
        if name_b.endswith(b"_inner"):
            structs = (arc56 or {}).get("structs") or {}
            fields = structs.get(str(vtype or "")) or []
            inner_t = fields[0].get("type") if fields else None
            abi_t = _abi_type_for(inner_t, arc56) if inner_t else None
            if abi_t:
                try:
                    from algosdk import abi as _abi
                    dec = _abi.ABIType.from_string(abi_t).decode(raw)
                    vals = [len(m) if isinstance(m, list) else _canon_abi(m)
                            for m in (dec if isinstance(dec, list) else [dec])]
                    # The EVM leg's struct view omits mapping members (solc
                    # layout skips them); the arc56 twin carries flattened
                    # placeholders that read as trailing zeros — trim them.
                    while len(vals) > 1 and not vals[-1]:
                        vals.pop()
                    return vals
                except Exception:
                    pass
            return None
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

    # Key hints mirroring the EVM leg's derivations from the SAME calls.json:
    # ordered (sender, arg_i, arg_j) triples for 3-deep mappings (Permit2's
    # allowance) and small/arg-derived uint words for mapping(addr => mapping
    # (uint => V)) (nonceBitmap). Same construction as evm_leg — key strings
    # must align for the differ to compare entries one-for-one.
    from chd_common import symbol as _symbol

    def _syms_in(v):
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            yield _symbol(v["__addr__"])
        elif isinstance(v, list):
            for x in v:
                yield from _syms_in(x)

    triples: list = []
    uint_keys: list = [0, 1, 2, 3]
    for c in (calls or []):
        if c.get("sender") and c.get("args"):
            s_sym = _symbol(c["sender"]["__addr__"])
            seen_args = [t for a in c["args"] for t in _syms_in(a)]
            for i2 in range(len(seen_args)):
                for j2 in range(len(seen_args)):
                    if i2 == j2:
                        continue
                    tri = (s_sym, seen_args[i2], seen_args[j2])
                    if tri not in triples:
                        triples.append(tri)
        for a in (c.get("args") or []):
            if isinstance(a, int) and 0 <= a < (1 << 256):
                for cand in (a >> 8, a):
                    if 0 <= cand < 4096 and cand not in uint_keys:
                        uint_keys.append(cand)

    matched = set()
    for mapname, mspec in bmaps.items():
        vtype = mspec.get("valueType")
        m = mapname.encode()
        got = {}
        def _hit(nm):
            """The chain box, or its mapping-carrying-struct "_inner" twin."""
            if nm in have:
                return nm
            if nm + b"_inner" in have:
                return nm + b"_inner"
            return None

        # Hex-form symbols first so a bytes32 key and an address symbol with
        # the SAME 32-byte content (zero role vs «Z») label the box the way
        # the EVM leg does; a box already claimed is not re-labeled.
        ordered_syms = sorted(
            syms.items(), key=lambda kv: 0 if str(kv[0]).startswith("0x") else 1)
        claimed: set = set()
        for sym, k in ordered_syms:                       # depth 1
            nm = _hit(hashlib.sha256(k + m).digest())
            if nm and nm not in claimed:
                claimed.add(nm)
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
        if not got:                                       # depth 3 (triples)
            for (t1, t2, t3) in triples:
                k1, k2, k3 = syms.get(t1), syms.get(t2), syms.get(t3)
                if k1 is None or k2 is None or k3 is None:
                    continue
                nm = hashlib.sha256(k3 + hashlib.sha256(
                    k2 + hashlib.sha256(k1 + m).digest()).digest()).digest()
                if nm in have:
                    matched.add(nm)
                    v = val_of(nm, vtype)
                    if _nonempty(v):
                        got[f"{t1}->{t2}->{t3}"] = v
        if not got:                                       # addr -> uint word
            for s1, k1 in syms.items():
                inner = hashlib.sha256(k1 + m).digest()
                for w in uint_keys:
                    nm = hashlib.sha256(
                        w.to_bytes(32, "big") + inner).digest()
                    if nm in have:
                        matched.add(nm)
                        v = val_of(nm, vtype)
                        if _nonempty(v):
                            got[f"{s1}->#{w}"] = v
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


def read_avm_maps(algod, app_id, arc56, layout, syms, fold, calls=None,
                  fns=None, app_id_symbols=None):
    """Read native box state through the recursive solc/ARC-56 type tree."""
    try:
        names = [base64.b64decode(item["name"])
                 for item in (algod.application_boxes(app_id).get("boxes") or [])]
    except Exception as exc:
        return {"__error__": str(exc)[:80]}

    box_values = {}
    for name in names:
        try:
            box_values[name] = base64.b64decode(
                (algod.application_box_by_name(app_id, name) or {}).get("value") or "")
        except Exception:
            continue

    from Crypto.Hash import keccak as _keccak_mod
    def _keccak(data):
        digest = _keccak_mod.new(digest_bits=256)
        digest.update(data)
        return digest.digest()

    extras = bytes32_mapping_key_candidates(calls or [], fns or {}, _keccak)
    evidence = KeyEvidence(calls or [], fns or {}, syms, extras)
    reader = NativeStorageReader(
        layout, arc56, box_values, evidence,
        lambda data: hashlib.sha256(data).digest(), fold)
    out = reader.read_maps()

    raw_slots = {}
    raw_names = set()
    for name, value in box_values.items():
        if name.startswith(b"s:") and len(name) == 34:
            slot = str(int.from_bytes(name[2:], "big"))
            number = int.from_bytes(value, "big") if value else 0
            if number in (app_id_symbols or {}):
                raw_slots[slot] = app_id_symbols[number]
            elif len(value) == 32:
                folded = fold(value)
                raw_slots[slot] = (folded if not str(folded).startswith("?")
                                   else number)
            else:
                raw_slots[slot] = number
            raw_names.add(name)

    box_keys = (((arc56.get("state") or {}).get("keys") or {}).get("box") or {})
    roots = {base64.b64decode(spec["key"])
             for spec in box_keys.values() if spec.get("key")}
    # A declared map's own name is the prefix every derived key chains from, so
    # the root box is accounted for by definition — counting it as
    # unattributed reported babydoge's `_balances`/`_allowances` as coverage
    # holes on a case with no divergences at all.
    roots |= {name.encode()
              for name in (((arc56.get("state") or {}).get("maps") or {})
                           .get("box") or {})}
    unexplained = set(box_values) - reader.matched - raw_names - roots
    groups = {}
    for name in sorted(unexplained):
        shape = f"name:{len(name)}/value:{len(box_values[name])}"
        item = groups.setdefault(shape, {"boxes": 0, "sample": []})
        item["boxes"] += 1
        if len(item["sample"]) < 8:
            item["sample"].append("0x" + name.hex())
    out["__unattributed_boxes__"] = len(unexplained)
    out["__unattributed_box_groups__"] = groups
    out["__raw_slots__"] = raw_slots
    out["__coverage__"] = {
        "boxes_total": len(box_values),
        "typed_boxes": len(reader.matched),
        "raw_slots": len(raw_names),
        "root_boxes": len(roots & set(box_values)),
        "unattributed": len(unexplained),
    }
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

    mut = {}
    for e in case["abi"]:
        if e.get("type") == "function":
            sig = e["name"] + "(" + ",".join(
                _ctype(i) for i in e["inputs"]) + ")"
            mut[sig] = e.get("stateMutability", "")

    ln = LocalNet()
    dispenser = ln.account
    h = Harness(ln, case_dir / "out_avm")

    # Anchor LocalNet BEFORE sender funding or dependency deployment. In dev
    # mode, the first ordinary transaction otherwise adopts wall time and can
    # outrun a requested historical/shifted base before BlockClock is created.
    # The EVM leg records the target constructor's actual deployment timestamp,
    # so both main constructors observe the same instant as well.
    clock = BlockClock(ln.algod, dispenser,
                       enabled=bool(opts.get("pin_time", True)))
    time_base = int(opts.get("time_base") or 0)
    deployment_time = int(opts.get("deployment_time") or time_base)
    clock.advance_to(deployment_time)
    clock_by_index = replay_clock_targets(calls, time_base)

    # ── deterministic sender accounts, funded from the dispenser ──────────
    accts = {i: algo_account(i) for i in reg["senders"].values()}
    # ARG-SYMBOL senders: a contract that only ever appears as an argument
    # (the curator Safe behind morpho's internal setAdmin/setFlowCaps) still
    # SENDS internal calls. accts.get(sym, dispenser) silently degraded those
    # to the dispenser while the tape translation used the arg-content form —
    # the owner()==msg.sender compare could never pass. Materialise a real
    # account for every sender symbol, whatever registry class it came from.
    for _c in calls:
        _sm0 = (_c.get("sender") or {}).get("__addr__")
        if isinstance(_sm0, int) and _sm0 not in accts:
            accts[_sm0] = algo_account(_sm0)
    used = {c["sender"]["__addr__"] for c in calls
            if c.get("sender") and isinstance(c["sender"].get("__addr__"), int)}
    algod = ln.algod
    # LocalNet mines a block PER TXN, so a once-fetched suggested_params goes
    # stale after ~1000 payments ("txn dead: round X outside of Y--Z") — which
    # broke every deep replay with >1000 senders. Refresh periodically.
    # Fee float PLUS the scaled value this sender pays out across the window.
    # calls.json carries the amount already scaled (chd_common.scale_value), so
    # both legs move the same number. Size the float from the framework's real
    # worst-case raw-call retry group. A fixed float made clean runs fail after
    # enough calls while repeated runs passed because deterministic senders
    # retained funding from earlier runs.
    owed = {}
    fee_float = {}
    for c in calls:
        v = int(c.get("value") or 0)
        m = (c.get("sender") or {}).get("__addr__")
        if isinstance(m, int):
            if v:
                owed[m] = owed.get(m, 0) + v
            if not c.get("skip") and c.get("i") not in ext_skips:
                fee_float[m] = fee_float.get(m, 0) + raw_call_fee_ceiling(
                    extra_fee=20_000, payment_wei=v)
    sp = algod.suggested_params()
    last, sent = None, 0
    for i, a in accts.items():
        if i not in used:
            continue
        if sent and sent % 200 == 0:
            sp = algod.suggested_params()
        txn = PaymentTxn(dispenser.address, sp, a.address,
                         5_000_000 + fee_float.get(i, 0) + owed.get(i, 0))
        last = algod.send_transaction(txn.sign(dispenser.private_key))
        sent += 1
    if last:
        wait_for_confirmation(algod, last, 8)
    print(f"[avm] funded {len(used)} sender account(s)")

    dep_local = {}                # hist addr → AVM app-id address form

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
        if isinstance(v, dict) and set(v) == {"__dep__"}:
            return dep_local[v["__dep__"]]
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            return concrete_addr(v["__addr__"])
        if isinstance(v, dict) and set(v) == {"__b__"}:
            return bytes.fromhex(v["__b__"])
        if isinstance(v, list):
            return [resolve(x) for x in v]
        return v

    # ── constructor DEPENDENCIES: deploy each (children-first order from the
    # EVM leg's meta) and map its historical address to the app-id address
    # form (bzero(24) ++ itob(app_id)) — the puya-sol cross-contract calling
    # convention, so the main ctor's inner txns reach the local dep.
    evm_layout = bool(opts.get("evm_layout"))
    evm_memory = bool(opts.get("evm_memory"))
    # --evm-layout now maps to the compiler's UMBRELLA flag (storage + memory
    # + transient coherence). The 39-contract zero-divergence certification
    # predates this; a re-certification run revalidates sizes/behavior.
    _mode_args = ([] + (["--evm-layout"] if evm_layout else [])
                     + (["--evm-memory-layout"] if (evm_memory and not evm_layout) else [])) or None
    split_config = opts.get("split_config")
    if split_config:
        split_path = Path(split_config)
        if not split_path.is_absolute():
            split_path = case_dir / split_path
        _main_args = list(_mode_args or []) + ["--split-config", str(split_path)]
    else:
        _main_args = _mode_args
    force_delegate = opts.get("force_delegate") or []
    if isinstance(force_delegate, str):
        force_delegate = [x.strip() for x in force_delegate.split(",") if x.strip()]
    if force_delegate:
        _main_args = list(_main_args or []) + [
            "--force-delegate", ",".join(force_delegate)]
    # These cases are fetched from deployed EVM contracts. Their public entry
    # boundary and typed cross-contract calls therefore use canonical Solidity
    # calldata; ARC4 remains the native transport only for harness-private
    # lifecycle methods and dependency tape loaders.
    _main_args = list(_main_args or []) + ["--contract-abi", "evm"]
    # A dep that must ROUTE a call has to be built with its CALLER's wire ABI.
    # Deps were compiled with the mode flags only, so their routers dispatched
    # ARC-4 method selectors while the contract under test called them with EVM
    # 4-byte ones: no route matched, the stub's catch-all fallback answered with
    # its own address, and the caller's bool decode failed ("invalid EVM ABI
    # bool") -- 278 status divergences on cctp_messenger's AVM leg alone, purely
    # a build-flag mismatch.
    #
    # But ONLY for deps with no answer tape. A TAPE-DRIVEN dep is meant to be
    # answered from its `__fallback`, which is where an unmatched selector lands
    # either way, so its router ABI is irrelevant -- and building it EVM-side
    # would strip the ARC-4 configs the harness needs to LOAD that tape
    # (`__load`/`__seek` survive as ARC-4 routes only in the default profile;
    # under --contract-abi evm a dep exposes `__postInit()` and nothing else,
    # which broke every one of Aave's 18 deps with "method not found in
    # app_spec"). Split-config/force-delegate stay main-contract-only.
    _tape_path = case_dir / "dep_tape.json"
    _taped_deps = {a.lower() for a in
                   (((load_json(_tape_path) or {}).get("tapes") or {})
                    if _tape_path.exists() else {})}
    dep_apps = []
    dep_app_byaddr = {}
    for dspec in meta.get("dep_ctors") or []:
        dep_sol = case_dir / dspec["dir"] / "prepared.sol"
        try:
            try:
                _abi_args = (_mode_args
                             if dspec["addr"].lower() in _taped_deps
                             else list(_mode_args or []) + ["--contract-abi", "evm"])
                darts = h.compile(dep_sol, extra_args=_abi_args)
                dapp = h.deploy(darts, dspec.get("name"),
                                ctor_args=[resolve(m) for m in dspec["args"]] or None)
            except Exception:
                _fb = case_dir / dspec["dir"] / "stub_fallback.sol"
                if not _fb.exists():
                    raise
                darts = h.compile(_fb, extra_args=_abi_args)
                dapp = h.deploy(darts, "StubERC20")
                print(f"[avm] dep {dspec.get('name')}: generic stand-in "
                      f"deployed instead (real dep failed on this leg)")
        except Exception as e:
            print(f"[avm] dep {dspec.get('name')}: {str(e)[:120]} — "
                  f"main ctor will fail as before")
            continue
        dep_local[dspec["addr"]] = encoding.encode_address(
            bytes(24) + dapp.app_id.to_bytes(8, "big"))
        dep_apps.append(dapp)
        ensure_app_funded(algod, dispenser, dapp.app_addr)
        print(f"[avm] dep {dspec.get('name')} app_id={dapp.app_id}")
        dep_app_byaddr[dspec["addr"].lower()] = dapp

    # TWO-PHASE tape load: all stubs exist now, so answers can be translated
    # into THIS leg's address space (historical 20-byte content → full 32-byte
    # local word: senders/creator become real accounts, args their content
    # form, deps the local stub address).
    _m20 = {}
    _sender_syms = {(_c.get("sender") or {}).get("__addr__") for _c in calls}
    for _a, _i in (reg.get("args") or {}).items():
        # symbols that SEND resolve to their real (signable) account — the
        # translated answer must equal what msg.sender will actually be
        _m20[bytes.fromhex(_a[2:])] = (
            encoding.decode_address(accts[_i].address)
            if _i in _sender_syms and _i in accts
            else bytes(12) + arg_content20(_i))
    for _a, _i in (reg.get("senders") or {}).items():
        if _i in accts:
            _m20[bytes.fromhex(_a[2:])] = encoding.decode_address(accts[_i].address)
    if reg.get("creator"):
        _m20[bytes.fromhex(reg["creator"][2:])] = encoding.decode_address(
            dispenser.address)
    for _a, _loc in dep_local.items():
        _m20[bytes.fromhex(_a[2:])] = encoding.decode_address(_loc)
    dep_plans = build_dep_tape_plans(case_dir, set(), _m20, calls=calls)
    _dep_seek = {}
    for _a, _plan in dep_plans.items():
        _dapp = dep_app_byaddr.get(_a)
        if not _dapp or not _plan["answers"]:
            continue
        for _ws, _ls, _sels in tape_script_chunks(
                _plan["answers"], _plan["selectors"]):
            h.call(_dapp, "__load(bytes32[],uint256[],bytes32[])",
                   _ws, _ls, _sels,
                   extra_fee=10_000)
        print(f"[avm] dep tape loaded: {len(_plan['answers'])} answer(s) "
              f"@ {_a[:10]}…")
        import os as _os
        if _os.environ.get("CHD_TAPE_DEBUG"):
            print(f"[avm] tape-head {_a[:10]} app={_dapp.app_id} head="
                  + " | ".join(a.hex()[:48]
                               for a in _plan["answers"][:3]))
        _dep_seek[_a] = (_dapp, _plan["bounds"])

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
                              extra_args=_main_args)
    else:
        artifacts = h.compile(case_dir / "prepared.sol", extra_args=_main_args)

    artifact_root = next(iter(artifacts.by_contract.values()))["arc56"].parents[1]
    compiler_events = []
    seen_compiler_events = set()
    event_spec_paths = [artifact["arc56"]
                        for artifact in artifacts.by_contract.values()]
    # The unsplit contract spec remains beside the per-compilation directory.
    # It is the authoritative union of events before public methods are moved
    # to internal-only page contracts (whose individual specs intentionally
    # omit those events).
    unsplit_spec = artifact_root.parent / f"{case['name']}.arc56.json"
    if unsplit_spec.exists():
        event_spec_paths.append(unsplit_spec)
    for spec_path in event_spec_paths:
        spec = load_json(spec_path)
        for event in spec.get("events") or []:
            key = (event.get("name"), tuple(
                (arg.get("name"), arg.get("type"))
                for arg in event.get("args") or []))
            if key not in seen_compiler_events:
                seen_compiler_events.add(key)
                compiler_events.append(event)
    delegate_doc_path = artifact_root / "delegate_helpers.json"
    delegate_doc = (load_json(delegate_doc_path)
                    if delegate_doc_path.exists() else {}) or {}
    delegate_pages = {
        entry["method"]: entry["contract_name"]
        for entry in delegate_doc.get("delegates") or []
    }

    # SimpleSplitter emits one artifact directory per helper plus the final
    # orchestrator. Deploy helpers in dependency order, deriving that order
    # from the emitted template references rather than from contract-specific
    # knowledge, then substitute the complete helper-id map into the main app.
    import re as _re
    helper_names = {n for n in artifacts.by_contract if "__Helper" in n}
    helper_apps = {}
    pending_helpers = set(helper_names)
    while pending_helpers:
        progressed = False
        for helper_name in sorted(pending_helpers):
            art = artifacts.by_contract[helper_name]
            teal = art["approval_teal"].read_text()
            refs = set(_re.findall(
                r"TMPL_([A-Za-z0-9_]+__Helper\d+)_APP_ID", teal))
            if (refs & helper_names) - set(helper_apps):
                continue
            tvars = {f"TMPL_{n}_APP_ID": a.app_id
                     for n, a in helper_apps.items()}
            happ = h.deploy(artifacts, helper_name, template_values=tvars)
            ensure_app_funded(algod, dispenser, happ.app_addr)
            helper_apps[helper_name] = happ
            pending_helpers.remove(helper_name)
            progressed = True
            print(f"[avm] split helper {helper_name} app_id={happ.app_id}")
            break
        if not progressed:
            raise RuntimeError("split helper dependency cycle: "
                               + ", ".join(sorted(pending_helpers)))

    split_tvars = {f"TMPL_{n}_APP_ID": a.app_id
                   for n, a in helper_apps.items()}

    proxy_runtime = bool((case.get("proxy") or {}).get("initializer"))
    if proxy_runtime:
        main_artifact = artifacts.by_contract.get(case["name"])
        main_arc56 = load_json(main_artifact["arc56"]) if main_artifact else {}
        methods = ((main_arc56.get("methods") or [])
                   if isinstance(main_arc56, dict) else [])
        if not any(m.get("name") == "__postInit" for m in methods):
            raise RuntimeError(
                "proxy-runtime replay requires a deferred constructor; "
                "this implementation executed constructor storage during "
                "AppCreate and cannot be modelled safely")

    # Opcode-budget pool for __postInit. A real contract's constructor can be
    # far heavier than one txn's 700 opcodes — VANRY's costs 6292 and failed
    # with "dynamic cost budget exceeded", which surfaces as a deploy error and
    # reads like a miscompile. Each pooled txn adds 700; the pool shares the
    # 16-txn GROUP with the postInit call itself, so this cannot go much
    # higher (at 16 the composer rejects the group outright).
    app = h.deploy(artifacts, case["name"],
                   ctor_args=[resolve(m) for m in meta["ctor_args"]] or None,
                   postinit_budget_pool=12,
                   postinit_inner_txns=4 * len(dep_apps),
                   skip_postinit=proxy_runtime,
                   reserve_program_pages=7 if delegate_pages else 0,
                   template_values=split_tvars or None)
    print(f"[avm] deployed {case['name']} app_id={app.app_id}")
    ensure_app_funded(algod, dispenser, app.app_addr)     # headroom for box MBR
    topups = 0

    main_artifact = artifacts.by_contract[case["name"]]

    def _evm_wire_value(value, spec):
        """Convert a resolved replay value to eth-abi's recursive shape."""
        typ = spec.get("type", "")
        m = re.match(r"^(.*)\[(\d*)\]$", typ)
        if m:
            elem = dict(spec)
            elem["type"] = m.group(1)
            return [_evm_wire_value(item, elem) for item in (value or [])]
        if typ == "tuple":
            return tuple(_evm_wire_value(item, component)
                         for item, component in zip(
                             value, spec.get("components") or []))
        if typ == "address":
            if isinstance(value, str):
                try:
                    raw = encoding.decode_address(value)
                except Exception:
                    raw = bytes.fromhex(value.removeprefix("0x"))
            else:
                raw = bytes(value)
            return raw[-20:]
        if typ.startswith("bytes"):
            return bytes(value)
        return value

    def _evm_selector(sig):
        from Crypto.Hash import keccak
        digest = keccak.new(digest_bits=256)
        digest.update(sig.encode())
        return digest.digest()[:4]

    def _call_evm(app_, sig, args, **call_opts):
        fn = meta["fns"].get(sig) or {"inputs": [], "outputs": []}
        inputs = fn.get("inputs") or []
        values = [_evm_wire_value(value, spec)
                  for value, spec in zip(args, inputs)]
        body = evm_abi_encode([_ctype(spec) for spec in inputs], values)
        result = h.call_raw(
            app_, _evm_selector(sig), extra_args=(body,), **call_opts)
        if result.reverted:
            return result
        magic = bytes.fromhex("151f7c75")
        return_payload = next(
            (bytes(log)[4:] for log in reversed(result.logs)
             if bytes(log).startswith(magic)), None)
        outputs = fn.get("outputs") or []
        if return_payload is None:
            raise RuntimeError(f"EVM entry {sig} returned no structured payload")
        decoded = (list(evm_abi_decode(
            [_ctype(spec) for spec in outputs], return_payload))
                   if outputs else [])
        result.abi_return = (decoded[0] if len(decoded) == 1
                             else tuple(decoded) if decoded else None)
        return result

    def invoke(sig, args, call_index=None, **call_opts):
        """Run a method in its state-preserving code page when configured."""
        page_name = delegate_pages.get(sig.split("(", 1)[0])
        if not page_name:
            return _call_evm(app, sig, args, **call_opts)
        update_program(
            ln, app.app_id, artifacts.by_contract[page_name],
            split_tvars or None, account=dispenser)
        if call_index is not None:
            try:
                block_no[str(call_index)] = int(algod.status()["last-round"]) + 1
            except Exception:
                pass
        try:
            return _call_evm(app, sig, args, **call_opts)
        finally:
            update_program(
                ln, app.app_id, main_artifact,
                split_tvars or None, account=dispenser)

    # inverse fold: 32-byte content hex → registry symbol
    inv = {encoding.decode_address(dispenser.address).hex(): symbol("C"),
           encoding.decode_address(app.app_addr).hex(): symbol("self"),
           bytes(32).hex(): symbol("Z")}
    for i, a in accts.items():
        inv[encoding.decode_address(a.address).hex()] = symbol(i)
    for _a, i in reg["args"].items():
        inv[(bytes(12) + arg_content20(i)).hex()] = symbol(i)
    for _a, i in (reg.get("deps") or {}).items():
        if _a in dep_local:
            inv[encoding.decode_address(dep_local[_a]).hex()] = symbol(f"D{i}")
    # A Solidity `address` occupies 20 bytes of an EVM slot, so under
    # --evm-storage-layout a 32-byte AVM account is stored TRUNCATED to its low
    # 20 bytes. Without these entries such a slot folds to a raw "?0x…" while
    # the EVM leg folds its own to a symbol, and the differ reports two
    # renderings of the SAME account as a divergence (staup `_owner`).
    for _k in list(inv):
        _trunc = (bytes(12) + bytes.fromhex(_k)[-20:]).hex()
        inv.setdefault(_trunc, inv[_k])

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
        if len(hx) == 40:
            hx = bytes(12).hex() + hx
        return inv.get(hx, f"?0x{hx}")

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

    def fold_events(raw_logs):
        """decode_avm_logs canonicalises addresses to 32-byte hex; re-fold those
        to registry symbols, and normalise bytesN, so both legs compare like
        for like."""
        got = decode_avm_log_bytes(
            raw_logs,
            [e for e in case["abi"] if e.get("type") == "event"],
            compiler_events)
        out = []
        for lg in got:
            ins = lg.get("inputs") or ev_types.get(lg["name"], [])
            args = [canon_value(v, i2.get("type", "uint256"), fold,
                                i2.get("components"))
                    for v, i2 in zip(lg["args"], ins)]
            out.append({"name": lg["name"], "args": args})
        return out

    # ── replay ────────────────────────────────────────────────────────────
    results, snapshots, platform_limits = {}, {}, {}
    snapshot_at = set(meta["snapshot_at"])
    block_ts, block_no = {}, {}
    active_dep_tapes = set()
    for c in calls:
        i = c["i"]
        if i % 25 == 0 and ensure_app_funded(algod, dispenser, app.app_addr):
            topups += 1
        if not c.get("skip") and i not in ext_skips:
            clock.advance_to(clock_by_index.get(i))
            if clock.cur is not None:
                block_ts[str(i)] = clock.cur
            # Round (chain height) at this txn: a contract storing block.number
            # writes each leg's own height; the differ absorbs that skew only
            # with the height at the writing txn on record. +1: the call lands
            # in the NEXT round after this status read.
            try:
                block_no[str(i)] = int(algod.status()["last-round"]) + 1
            except Exception:
                pass
            sig, args = c["sig"], [resolve(a) for a in c["args"]]
            is_view = mut.get(sig, "") in ("view", "pure")
            # Activate only this transaction's selector-bounded scripts. Clear
            # dependencies used by the previous transaction when necessary;
            # untouched stand-ins need no extra setup transaction.
            next_dep_tapes = {
                _a3 for _a3, (_, _bounds2) in _dep_seek.items()
                if i in _bounds2
            }
            for _a3 in active_dep_tapes | next_dep_tapes:
                _dapp2, _bounds2 = _dep_seek[_a3]
                start, end = _bounds2.get(i, (0, 0))
                try:
                    h.call(_dapp2, "__seek(uint256,uint256)", start, end,
                           extra_fee=10_000)
                    import os as _os
                    if _os.environ.get("CHD_TAPE_DEBUG") and i < 2 and end > start:
                        _idxr = h.call(_dapp2, "__idx()", extra_fee=10_000)
                        _endr = h.call(_dapp2, "__end()", extra_fee=10_000)
                        _selr = h.call(
                            _dapp2, "__selectors(uint256)", start,
                            extra_fee=10_000)
                        _lenr = h.call(
                            _dapp2, "__lens(uint256)", start,
                            extra_fee=10_000)
                        _selv = bytes(_selr.abi_return).hex()
                        print(f"[avm] TAPE-PROBE {_a3[:10]} app={_dapp2.app_id} "
                              f"i={i} idx={_idxr.abi_return} end={_endr.abi_return} "
                              f"selector={_selv[:8]} len={_lenr.abi_return}")
                        _raw = h.call_raw(
                            _dapp2, bytes(_selr.abi_return)[:4],
                            extra_fee=10_000)
                        print(f"[avm] TAPE-PROBE   raw-ok={not _raw.reverted} "
                              f"logs={[bytes(x).hex() for x in _raw.logs]}")
                        # The probe is a committed transaction. Restore the
                        # transaction-bounded cursor before the real call.
                        h.call(_dapp2, "__seek(uint256,uint256)", start, end,
                               extra_fee=10_000)
                except Exception as _se:
                    # A silently-lost seek can serve a stale answer range.
                    print(f"[avm] SEEK FAILED {_a3[:10]} i={i}: "
                          f"{str(_se)[:90]}")
            active_dep_tapes = next_dep_tapes
            prev = ln.account
            _sm = (c.get("sender") or {}).get("__addr__")
            ln.account = accts.get(_sm, dispenser) if isinstance(_sm, int) else dispenser
            try:
                # Fee headroom ALWAYS, not just when ctor deps exist: any
                # contract may issue inner txns (friendtech's buyShares sends
                # protocol+subject fees via .call{value} = 2 inner payments),
                # and without pooled fee the budget-pool retry group comes up
                # exactly the inner-txn fees short — the call then dies and the
                # recorded reason is whatever the FIRST unpopulated submit said
                # ("invalid Box reference"), which reads as a resource bug.
                dep_fee = {"extra_fee": 20_000}
                # Tape-serving inner calls read the STUB's page boxes, and the
                # 8-box-ref budget of a single txn is consumed by the target
                # app's own pages first. A missing box FAILS SOFT to zero in
                # slot mode: __lens.length reads 0, the fallback answers
                # self-address, and morpho's guard reverts — the caps=[] probe
                # passed while every real setFlowCaps failed. Pool txns widen
                # the group's shared resource budget.
                if dep_plans:
                    # Historical dependency replay already requires a grouped
                    # call so its boxes/apps can be discovered.  Allocate the
                    # largest legal opcode pool up front: storage-heavy paths
                    # can perform many Keccaks before the first external call,
                    # and a small speculative pool makes success depend on a
                    # later resource-population retry.  call_raw trims this to
                    # leave room for the target and an optional msg.value
                    # payment, so this is generic across both call shapes.
                    dep_fee["budget_pool"] = 15
                # msg.value: framework prepends a payment to the app address.
                _v = int(c.get("value") or 0)
                if _v:
                    dep_fee["payment_wei"] = _v
                if is_view:
                    r = invoke(sig, args, i, expect_revert=True, **dep_fee)
                    if r.reverted:
                        results[i] = {"ok": False,
                                      "revert": str(getattr(r, "fail_message", ""))[:160]}
                    else:
                        results[i] = {"ok": True, "ret": _ret(r, meta, sig, fold),
                                      "logs": []}
                else:
                    r = invoke(sig, args, i, **dep_fee)
                    if getattr(r, "reverted", False):
                        reason = str(getattr(r, "fail_message", "")
                                     or getattr(r, "raw_response", ""))[:200]
                        results[i] = {"ok": False, "revert": reason}
                        if is_platform_limit(reason):
                            platform_limits[i] = reason
                        import os as _os
                        if _os.environ.get("CHD_TAPE_DEBUG") and "err op" in reason:
                            print(f"[avm] FAIL-PROBE i={i} sender={ln.account.address}")
                            for _av in (c.get("args") or []):
                                if isinstance(_av, dict) and set(_av) == {"__dep__"}:
                                    _ad4 = _av["__dep__"].lower()
                                    _pair = _dep_seek.get(_ad4)
                                    if not _pair:
                                        print(f"[avm] FAIL-PROBE   {_ad4[:10]}: NO TAPE/SEEK")
                                        continue
                                    _dapp4, _sk4 = _pair
                                    try:
                                        _rr = h.call(_dapp4, "__idx()", extra_fee=10_000)
                                        print(f"[avm] FAIL-PROBE   {_ad4[:10]} app="
                                              f"{_dapp4.app_id} idx={_rr.abi_return} "
                                              f"want={_sk4.get(i)}")
                                    except Exception as _pe:
                                        print(f"[avm] FAIL-PROBE   probe err: {str(_pe)[:80]}")
                    else:
                        results[i] = {"ok": True, "ret": _ret(r, meta, sig, fold),
                                      "logs": fold_events(r.logs) or []}
            except Exception as e:
                reason = str(e)[:200]
                results[i] = {"ok": False, "revert": reason}
                if is_platform_limit(reason):
                    platform_limits[i] = reason
                import os as _os
                if _os.environ.get("CHD_TAPE_DEBUG") and "err op" in reason:
                    # Catch the guard red-handed: the sender this txn ACTUALLY
                    # used, and each __dep__-arg stub's answer at its seeked
                    # cursor (the failed txn rolled its consumption back, so
                    # re-serving reads the same entry the guard saw).
                    print(f"[avm] FAIL-PROBE i={i} sender={ln.account.address}")
                    for _av in (c.get("args") or []):
                        if isinstance(_av, dict) and set(_av) == {"__dep__"}:
                            _ad4 = _av["__dep__"].lower()
                            _pair = _dep_seek.get(_ad4)
                            if not _pair:
                                print(f"[avm] FAIL-PROBE   {_ad4[:10]}: NO TAPE/SEEK")
                                continue
                            _dapp4, _sk4 = _pair
                            try:
                                _rr = h.call(_dapp4, "__idx()",
                                             expect_revert=True, extra_fee=10_000)
                                print(f"[avm] FAIL-PROBE   {_ad4[:10]} app="
                                      f"{_dapp4.app_id} idx={_rr.abi_return} "
                                      f"want={_sk4.get(i)}")
                            except Exception as _pe:
                                print(f"[avm] FAIL-PROBE   probe err: {str(_pe)[:80]}")
            finally:
                ln.account = prev
        if i in snapshot_at:
            snap = {}
            for g in meta["getters"]:
                try:
                    r = invoke(g["sig"], [], expect_revert=True)
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

    # The artifact selected by Harness is the program actually deployed. A
    # stale top-level spec can survive a split compilation and describe a
    # different program/state schema, so it must never drive coverage.
    arc56 = load_json(main_artifact["arc56"])
    syms = {symbol("C"): encoding.decode_address(dispenser.address),
            symbol("Z"): bytes(32)}
    for _i, _a in accts.items():
        syms[symbol(_i)] = encoding.decode_address(_a.address)
    for _ad, _i in reg["args"].items():
        syms[symbol(_i)] = bytes(12) + arg_content20(_i)
    for _ad, _i in (reg.get("deps") or {}).items():
        if _ad in dep_local:
            syms[symbol(f"D{_i}")] = encoding.decode_address(dep_local[_ad])
    # Same shared instant the EVM leg pins to. Each AVM probe is a real
    # transaction sealing its own block, so without this the phase answers at
    # whatever the last replayed entry left behind and every accruing view
    # drifts against the other leg.
    probe_time = probe_clock_target(clock_by_index)
    clock.advance_to(probe_time)
    probe_results = {}
    for probe_index, probe in enumerate(meta.get("probes") or []):
        try:
            source_txn = probe.get("source_txn")
            if source_txn is not None:
                next_dep_tapes = {
                    addr for addr, (_dep, bounds) in _dep_seek.items()
                    if int(source_txn) in bounds}
                for addr in active_dep_tapes | next_dep_tapes:
                    dep, bounds = _dep_seek[addr]
                    start, end = bounds.get(int(source_txn), (0, 0))
                    h.call(dep, "__seek(uint256,uint256)", start, end,
                           extra_fee=10_000)
                active_dep_tapes = next_dep_tapes
            result = invoke(
                probe["sig"], [resolve(value) for value in probe.get("args") or []],
                expect_revert=True)
            if result.reverted:
                probe_results[str(probe_index)] = {
                    "ok": False,
                    "revert": str(getattr(result, "fail_message", ""))[:160]}
            else:
                value = result.abi_return
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

    if evm_layout:
        # --evm-storage-layout: storage IS solc's slot layout in boxes — read
        # it exactly the way the EVM leg reads py-evm state (chd_slot_reader).
        from chd_slot_reader import read_slot_map, read_slot_storage
        slot_layout = load_json(case_dir / "storage_layout.json")
        storage = read_slot_storage(
            read_slot_map(algod, app.app_id), slot_layout, syms, fold, calls,
            meta.get("fns") or {})
    else:
        slot_layout = load_json(case_dir / "storage_layout.json")
        storage = read_avm_storage(algod, app.app_id, arc56, fold)
        app_id_symbols = {app.app_id: symbol("self")}
        app_id_symbols.update({
            dep.app_id: symbol(f"D{reg['deps'][addr]}")
            for addr, dep in dep_app_byaddr.items()
            if addr in (reg.get("deps") or {})})
        maps = read_avm_maps(
            algod, app.app_id, arc56, slot_layout, syms, fold, calls,
            meta.get("fns") or {}, app_id_symbols)
        storage["raw_slots"] = maps.pop("__raw_slots__", {})
        storage["coverage"] = maps.pop("__coverage__", {})
        storage["maps"] = maps
    dump_json(case_dir / "avm_results.json",
              {"results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "probes": probe_results,
               "storage": storage,
               "platform_limits": {str(k): v for k, v in platform_limits.items()},
               "block_ts": block_ts,
               "block_no": block_no,
               "probe_time": probe_time,
               "app_id": app.app_id})
    n = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[avm] replayed {n} txns ({n_ok} ok, {n-n_ok} reverted, "
          f"{len(platform_limits)} platform-limit, {topups} app top-up(s), "
          f"{clock.jumps} clock jump(s))")


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
