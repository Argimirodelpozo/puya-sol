"""Shared utilities for chainwide-historical-diff (plain-python side).

Everything here must run under BOTH the system python3 (algosdk available) and
the tiny-fuzzing-oracle .evmvenv python (web3 available) — so no imports of
either ecosystem at module level; only stdlib.
"""
from __future__ import annotations

import hashlib
import json
import re
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]                                   # puya-sol repo root
CASES = HERE / "cases"
ORACLE_DIR = REPO / "tests" / "WIP" / "tiny-fuzzing-oracle"
SEMTESTS_DIR = REPO / "tests" / "solidity-semantic-tests"
EVM_PY = ORACLE_DIR / ".evmvenv" / "bin" / "python"

UA = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}

# Registry symbol space: senders 0.., arg-only addresses ARG_BASE..
ARG_BASE = 10000


def http_json(url: str, timeout: int = 40):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def load_json(p: Path):
    with open(p) as fh:
        return json.load(fh)


def dump_json(p: Path, obj):
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "w") as fh:
        json.dump(obj, fh, indent=1, default=_json_default)


def _json_default(o):
    if isinstance(o, (bytes, bytearray)):
        return {"__b__": bytes(o).hex()}
    raise TypeError(f"not JSON-serialisable: {type(o).__name__}")


def relax_pragma(src: str) -> str:
    """Exact-pinned `pragma solidity 0.8.N;` → `^0.8.N` so solc 0.8.26 (EVM leg)
    accepts it. Both legs compile the SAME prepared source."""
    return re.sub(r"pragma solidity\s+(=)?0\.8\.(\d+)\s*;",
                  r"pragma solidity ^0.8.\2;", src)


# AVM platform limits (opcode budget, box-reference packing, program size):
# reverts for these reasons are NOT miscompiles, but they fork the state, so the
# orchestrator re-skips the txn symmetrically. (Cribbed from fuzz_state.)
def is_platform_limit(reason: str) -> bool:
    m = (reason or "").lower()
    return ("budget" in m or "opcode" in m or "dynamic cost" in m
            or "invalid box reference" in m or "unavailable box" in m
            or "unavailable resource" in m or "max_group_size" in m
            or ("exceed" in m and "group" in m)
            or "extra_pages" in m or "8kb" in m
            # app-account min-balance grows with box count on long replays;
            # a resource shortfall, never a miscompile (avm_leg tops up, this
            # is the backstop so it can never masquerade as a finding)
            or "below min" in m or "min balance" in m or "overspend" in m)


# ── Address registry (pure data; each leg derives its concrete forms) ──────

ZERO = "0x" + "00" * 20


def build_registry(creator: str, sender_addrs: list[str], arg_addrs: list[str]) -> dict:
    """addr(lower) → symbol index. creator → 'C'; zero-addr → 'Z' (implicit)."""
    creator = creator.lower()
    reg = {"creator": creator, "senders": {}, "args": {}}
    i = 0
    for a in sender_addrs:
        a = a.lower()
        if a == creator or a == ZERO or a in reg["senders"]:
            continue
        reg["senders"][a] = i
        i += 1
    j = ARG_BASE
    for a in arg_addrs:
        a = a.lower()
        if a == creator or a == ZERO or a in reg["senders"] or a in reg["args"]:
            continue
        reg["args"][a] = j
        j += 1
    return reg


def marker_for(reg: dict, addr: str):
    """Historical 0x-address → transportable marker."""
    a = addr.lower()
    if a == ZERO:
        return {"__addr__": "Z"}
    if a == reg["creator"]:
        return {"__addr__": "C"}
    if a in reg["senders"]:
        return {"__addr__": reg["senders"][a]}
    if a in reg["args"]:
        return {"__addr__": reg["args"][a]}
    return {"__addr__": f"?{a}"}        # unmapped (shouldn't happen for args)


def symbol(marker_i) -> str:
    return f"«{marker_i}»"


# Deterministic keys (both legs derive from the same seeds).

def evm_sender_privkey(i: int) -> str:
    return "0x" + hashlib.sha256(b"chd-evm-sender-%d" % i).hexdigest()


def algo_sender_seed(i: int) -> bytes:
    return hashlib.sha256(b"chd-algo-sender-%d" % i).digest()


def arg_content20(i: int) -> bytes:
    """20-byte content address for arg-only registry entries (never a real
    account; low-20 bytes shared between legs, AVM pads with 12 zero bytes)."""
    return b"\xcd" + b"\x00" * 15 + int(i).to_bytes(4, "big")


# ── Type-driven canonicalisation (shared shape; leg passes its own addr fold) ─

def canon_value(v, abi_type: str, fold_addr, components=None):
    """Fold a decoded value into a leg-independent JSON shape.
    fold_addr(raw) → symbol string for this leg's concrete address form."""
    m = re.match(r"^(.*)\[(\d*)\]$", abi_type)
    if m:
        return [canon_value(x, m.group(1), fold_addr, components) for x in (v or [])]
    if abi_type == "tuple":
        comps = components or []
        return [canon_value(x, c.get("type", "uint256"), fold_addr, c.get("components"))
                for x, c in zip(list(v), comps)]
    if abi_type == "address":
        return fold_addr(v)
    if abi_type.startswith("bytes") or abi_type in ("string",):
        if isinstance(v, str) and abi_type == "string":
            return v
        if isinstance(v, (bytes, bytearray)):
            return "0x" + bytes(v).hex()
        if isinstance(v, list):                      # algosdk byte[] shape
            return "0x" + bytes(v).hex()
        if isinstance(v, str):
            return v if v.startswith("0x") else "0x" + v
        return str(v)
    if abi_type == "bool":
        return bool(v)
    if abi_type.startswith(("uint", "int")):
        return int(v)
    return v
