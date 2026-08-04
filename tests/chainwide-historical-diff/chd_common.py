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


# wei → the unit each leg actually moves. ETH carries 18 decimals and ALGO 6,
# so a literal 1:1 replay is not a tuning question but a SUPPLY one: the whole
# AVM LocalNet holds 1.0e16 microAlgos, which covers 5.8 median ETH transfers
# (measured). At 1e12 a 1-ETH transfer becomes 1 ALGO and everything fits.
#
# The scale is applied IDENTICALLY ON BOTH LEGS, which is what keeps it sound:
#   * a PASS-THROUGH contract (Aave's gateway does `WETH.deposit{value:
#     msg.value}`) behaves the same at any scale, so it replays for real;
#   * a PRICE-COMPARING contract (`require(msg.value >= price)`, friend.tech)
#     sees a scaled msg.value against an unscaled price and reverts — but it
#     reverts on BOTH legs, so the pair still agrees and the closed-world
#     filter drops it. No false divergence either way; the coverage is simply
#     not gained for that shape.
# Set 1 for a literal replay when a contract's values are small enough to fund.
VALUE_SCALE = 10 ** 12

# AVM amount fields are uint64. Anything wider cannot be expressed at all.
_AVM_AMOUNT_MAX = 2 ** 64 - 1


def scale_value(wei: int) -> int | None:
    """wei → this leg's amount, or None when it cannot be represented.

    Rounds UP so a nonzero payment never becomes a free one: a contract
    gating on `msg.value > 0` must still see a payment."""
    wei = int(wei or 0)
    if wei <= 0:
        return 0
    scaled = -(-wei // VALUE_SCALE)          # ceil
    return None if scaled > _AVM_AMOUNT_MAX else scaled


def replay_epoch(calls) -> int:
    """The historical instant both legs treat as t=0 for the replay clock.

    Must be computed identically on both legs, so it is derived from
    calls.json alone — never from a leg's own chain state."""
    ts = [int(c["ts"]) for c in (calls or []) if c.get("ts")]
    return min(ts) if ts else 0


def clock_target(ts, epoch, base):
    """Map a historical instant onto the clock both legs actually run at.

    Both legs must observe the same block.timestamp, and LocalNet's dev-mode
    clock is strictly MONOTONIC: the offset endpoint is uint64, and once offset
    mode is on, setting 0 freezes the clock rather than re-zeroing it — so the
    AVM leg can never rewind to 2022. Instead both legs replay the historical
    DELTAS from a shared `base` at or ahead of the AVM chain's current time.

    Absolute epoch may shift; every duration a contract can observe (cooldowns,
    vesting, permit expiry) is preserved exactly. When the chain's clock still
    sits before the window, `base` IS the historical epoch and the replay runs
    at true historical time.
    """
    if not ts or not base:
        return None
    return int(base) + max(0, int(ts) - int(epoch or 0))


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
    """Normalise pragmas so BOTH legs' compilers accept the same source.

    Two forms need rewriting, for opposite reasons:

    * exact-pinned `pragma solidity 0.8.N;` → `^0.8.N`, so the EVM leg's solc
      (a different 0.8.x) accepts it.
    * upper-bound-only `pragma solidity <0.9.0;` → `^0.8.0`. puya-sol's bundled
      solc is a PRERELEASE build (0.8.35-develop.…), and semver excludes
      prereleases from a plain `<` range, so these are rejected there while the
      EVM leg's release solc accepts them — 27 of Polymarket CTFExchange's 46
      files are written this way.
    """
    src = re.sub(r"pragma solidity\s+(=)?0\.8\.(\d+)\s*;",
                 r"pragma solidity ^0.8.\2;", src)
    return re.sub(r"pragma solidity\s*<\s*0\.9\.0\s*;",
                  "pragma solidity ^0.8.0;", src)


# AVM platform limits (opcode budget, box-reference packing, program size):
# reverts for these reasons are NOT miscompiles, but they fork the state, so the
# orchestrator re-skips the txn symmetrically. (Cribbed from fuzz_state.)
def is_platform_limit(reason: str) -> bool:
    m = (reason or "").lower()
    # "opcode BUDGET", not bare "opcode": algod appends a disassembly field
    # (`opcodes=gtxns Amount; !; assert`) to EVERY logic-eval error, so the bare
    # substring matched genuine contract reverts and silently skipped them as
    # platform limits — hiding real divergences, the exact inverse of the
    # masquerade this predicate exists to prevent.
    return ("budget" in m or "opcode budget" in m or "dynamic cost" in m
            or "invalid box reference" in m or "unavailable box" in m
            or "unavailable resource" in m or "max_group_size" in m
            or ("exceed" in m and "group" in m)
            or "extra_pages" in m or "8kb" in m
            # app-account min-balance grows with box count on long replays;
            # a resource shortfall, never a miscompile (avm_leg tops up, this
            # is the backstop so it can never masquerade as a finding)
            or "below min" in m or "min balance" in m or "overspend" in m
            # 256 inner txns per top-level txn (AVM hard cap). A Solidity loop
            # making external calls (batch airdrop, multicall, sweep) hits a
            # ceiling the EVM doesn't have — a platform limit, never a
            # miscompile, so it must not be able to masquerade as a finding.
            or "too many inner transactions" in m)


# ── Address registry (pure data; each leg derives its concrete forms) ──────

ZERO = "0x" + "00" * 20


def build_registry(creator: str, sender_addrs: list[str], arg_addrs: list[str]) -> dict:
    """addr(lower) → symbol index. creator → 'C'; zero-addr → 'Z' (implicit)."""
    # Creator can be unknown (creation txn outside the window / not exposed by
    # the explorer); fall back to the first sender so `owner = msg.sender`
    # contracts still line up, else the zero address.
    creator = (creator or (sender_addrs[0] if sender_addrs else ZERO)).lower()
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
    # ctor dependency → the locally deployed instance. Only as a call TARGET:
    # if the dep also SENT txns it is in `senders` and keeps its registry
    # symbol, because the legs need a signable account for it.
    if a in (reg.get("deps") or {}) and a not in reg.get("senders", {}):
        return {"__dep__": a}
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
