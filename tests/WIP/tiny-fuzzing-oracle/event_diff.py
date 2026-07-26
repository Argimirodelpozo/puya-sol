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
import re

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


_VALUE_TYPE_RE = re.compile(r"^(address|bool|(u?int)(\d+)?|bytes([0-9]|[12][0-9]|3[0-2]))$")


def _is_value_type(t):
    """A type stored as ONE 32-byte word (indexed topic or non-indexed data word)."""
    return bool(_VALUE_TYPE_RE.match(t))


def _decode_evm_word(word, t):
    """Decode a 32-byte EVM log word to the same canon shape as evm_oracle._canon_log_val."""
    if len(word) < 32:
        word = word.rjust(32, b"\x00")
    if t == "address":
        return "0x" + word.hex()                          # 32-byte content hex (account / padded addr)
    if t == "bool":
        return 1 if int.from_bytes(word, "big") else 0
    m = re.match(r"^bytes(\d+)$", t)
    if m:
        return list(word[: int(m.group(1))])              # bytesN: left-aligned, first N bytes
    m = re.match(r"^(u?)int(\d*)$", t)
    if m:
        bits = int(m.group(2) or 256)
        signed = m.group(1) == ""
        v = int.from_bytes(word, "big")
        if signed and v >= (1 << (bits - 1)):
            v -= 1 << bits                                 # two's complement (topic is sign-extended)
        return v
    return int.from_bytes(word, "big")


def decode_raw_evm_logs(raw_response, solc_events):
    """Decode AVM raw EVM-STYLE logs (asm logN: `log(topic0 ++ indexed-topics(32B each)
    ++ data)`) — as emitted by Solady-style assembly events — into [{name, args}] in the
    same canon shape as the EVM oracle. Uses the solc event ABIs (name/inputs/indexed +
    keccak topic0 from introspect). VALUE-TYPE events only (each arg one 32-byte word);
    events with dynamic/aggregate args are skipped (like the ARC-28 decoder). Returns None
    if no decodable logs container (mirrors decode_avm_logs)."""
    results = getattr(raw_response, "abi_results", None)
    if results is None:
        return None
    reg = {}
    for e in solc_events or []:
        t0 = (e.get("topic0") or "").lower()
        if not t0 or e.get("anonymous"):
            continue
        if all(_is_value_type(i["type"]) for i in e["inputs"]):
            reg[t0] = e
    out = []
    for res in results:
        ti = getattr(res, "tx_info", None)
        if not isinstance(ti, dict):
            continue
        for b64 in ti.get("logs", []) or []:
            data = base64.b64decode(b64)
            if len(data) < 32:
                continue
            ev = reg.get(("0x" + data[:32].hex()).lower())
            if not ev:
                continue                                  # not one of our (value-type) events
            inputs = ev["inputs"]
            nidx = sum(1 for i in inputs if i.get("indexed"))
            body = data[32:]
            idx_words = [body[i * 32:(i + 1) * 32] for i in range(nidx)]
            rest = body[nidx * 32:]
            non_words = [rest[i * 32:(i + 1) * 32] for i in range(len(inputs) - nidx)]
            args, ii, jj = [], 0, 0
            ok = True
            for i in inputs:
                if i.get("indexed"):
                    if ii >= len(idx_words): ok = False; break
                    w = idx_words[ii]; ii += 1
                else:
                    if jj >= len(non_words): ok = False; break
                    w = non_words[jj]; jj += 1
                args.append(_decode_evm_word(w, i["type"]))
            if ok:
                out.append({"name": ev["name"], "args": args})
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
