#!/usr/bin/env python3
"""Decode AVM-emitted event logs to (name, [values]) for the differential oracle.

puya-sol lowers `emit E(args)` to a `log` opcode: a 4-byte ARC-28 selector
(sha512_256 of `E(<arc56 arg types>)`) followed by the ARC4-encoded tuple of the
args. The arc56 event registration (app_spec.events) gives the exact per-arg
ARC4 types, so we compute the same selector to identify each log and decode the
body as an ARC4 tuple. Values are canonicalised to the SAME shape the EVM oracle
emits (evm_oracle._canon_log_val): int→int, bool→0/1, bytesN/bytes→list of byte
values, address→32-byte content hex — so integer args compare width-agnostically
(a uint128 value registered as uint256 or uint64 still decodes to the same int),
catching VALUE miscompiles (sign, truncation, wrong arg) without flagging the
documented backing-width registration convention.
"""
import base64
import hashlib

from algosdk import abi

_RET_PREFIX = bytes.fromhex("151f7c75")   # ARC4 method-return log prefix


def _selector(name, arc56_types):
    sig = f"{name}({','.join(arc56_types)})"
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def _algosdk_type(t):
    # Decode address/account as raw 32 bytes (not algosdk's base32 address string).
    return "byte[32]" if t in ("address", "account") else t


def _canon(v, arc56_type):
    if isinstance(v, bool):
        return int(v)
    if arc56_type in ("address", "account") and isinstance(v, (bytes, bytearray)):
        return "0x" + bytes(v).hex()             # 32-byte content hex (matches EVM side)
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    if isinstance(v, (list, tuple)):
        return [_canon(x, "") for x in v]
    return v


def _event_registry(events):
    """app_spec.events → {selector_bytes: (name, [arc56_type_str])}."""
    reg = {}
    for e in events or []:
        types = [str(a.type) for a in e.args]
        reg[_selector(e.name, types)] = (e.name, types)
    return reg


def decode_avm_logs(raw_response, events):
    """Execute-response → [{name, args:[canon values]}] for each matched event log.

    Returns None if the raw_response carries no decodable logs container (e.g. a
    simulate/reverted result whose raw_response is a str) — distinct from [] (ran,
    emitted nothing)."""
    results = getattr(raw_response, "abi_results", None)
    if results is None:
        return None
    reg = _event_registry(events)
    out = []
    for res in results:
        ti = getattr(res, "tx_info", None)
        if not isinstance(ti, dict):
            continue
        for b64 in ti.get("logs", []) or []:
            data = base64.b64decode(b64)
            if data[:4] == _RET_PREFIX:
                continue
            ev = reg.get(data[:4])
            if not ev:
                continue                          # unknown selector — not one of our events
            name, types = ev
            try:
                tt = abi.TupleType([abi.ABIType.from_string(_algosdk_type(t)) for t in types])
                vals = tt.decode(data[4:])
            except Exception:
                continue                          # dynamic/opaque layout we can't tuple-decode
            out.append({"name": name, "args": [_canon(v, t) for v, t in zip(vals, types)]})
    return out


def _norm_cmp(v):
    """Normalise an arg to a form where the backing-width convention and the
    address/bytes representation don't false-diverge — applied IDENTICALLY to
    both sides, so equality is preserved regardless of signedness. Ints in the
    64-/256-bit signed range fold to their two's-complement negative (EVM -128
    and AVM 2^64-128 both → -128; a genuine large uint folds the same on both
    sides). Bytes / byte-lists / 0x-hex-strings all fold to one lowercase hex
    string (address hex-vs-bytelist repr gap)."""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        if (1 << 63) <= v < (1 << 64):
            return v - (1 << 64)
        if (1 << 255) <= v < (1 << 256):
            return v - (1 << 256)
        return v
    if isinstance(v, str) and v.startswith(("0x", "0X")):
        return "0x" + v[2:].lower()
    if isinstance(v, (bytes, bytearray)):
        return "0x" + bytes(v).hex()
    if isinstance(v, (list, tuple)):
        # a list of small ints that is really a byte string (address/bytesN
        # decoded to bytes on one side) → hex; otherwise recurse per element.
        if v and all(isinstance(x, int) and 0 <= x < 256 for x in v):
            return "0x" + bytes(v).hex()
        return [_norm_cmp(x) for x in v]
    return v


def logs_match(evm_logs, avm_logs):
    """Compare two [{name,args}] lists as multisets (emission order can differ; a
    NAME+args multiset is the semantically meaningful invariant). Returns
    (ok, evm_only, avm_only)."""
    def key(l):
        return (l["name"], repr([_norm_cmp(a) for a in l["args"]]))
    from collections import Counter
    ce, ca = Counter(key(l) for l in evm_logs), Counter(key(l) for l in avm_logs)
    evm_only = list((ce - ca).elements())
    avm_only = list((ca - ce).elements())
    return (not evm_only and not avm_only), evm_only, avm_only
