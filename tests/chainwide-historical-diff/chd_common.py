"""Shared utilities for chainwide-historical-diff (plain-python side).

Everything here must run under BOTH the system python3 (algosdk available) and
the tiny-fuzzing-oracle .evmvenv python (web3 available) — so no imports of
either ecosystem at module level; only stdlib.
"""
from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Mapping
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


def deployment_clock_target(creation_ts, calls, base):
    """Map contract creation through the same epoch shift as replay calls.

    Creation normally predates the first call, so ``clock_target`` cannot be
    reused: its forward-only clamp would collapse creation onto the first-call
    instant. Preserve that historical lead time when the chain is fresh, and
    apply the same positive shift to both when LocalNet has already advanced.
    """
    if not base:
        return int(creation_ts or 0)
    epoch = replay_epoch(calls)
    if not creation_ts or not epoch:
        return int(base)
    return max(61, int(base) + int(creation_ts) - epoch)


def replay_time_base(chain_now, creation_ts, calls):
    """Choose a call epoch whose shifted creation is still reachable.

    An already-advanced AVM chain cannot rewind to a creation timestamp that
    predates the first call. Move the call base ahead by that lead as well, so
    mapping creation through ``deployment_clock_target`` lands just after the
    current chain rather than behind it.
    """
    epoch = replay_epoch(calls)
    if not epoch:
        return max(0, int(chain_now or 0) + 1)
    creation = int(creation_ts or epoch)
    lead = max(0, epoch - creation)
    return max(epoch, int(chain_now or 0) + 1 + lead)


def replay_clock_targets(calls, base):
    """Return one deterministic, strictly increasing timestamp per entry.

    The harvested stream can contain several internal calls from one Ethereum
    transaction, so equal historical timestamps are common. Py-EVM mines each
    replay entry in its own block and necessarily advances that block's time;
    LocalNet can hold time fixed. Letting those defaults stand makes time-based
    contracts diverge according to harness speed. This schedule preserves every
    historical gap that is at least one second and applies the same minimal
    one-second tie break on both legs.

    Include skipped entries in the schedule so convergence passes do not change
    the timestamps assigned to later calls.
    """
    epoch = replay_epoch(calls)
    previous = int(base or 0) - 1
    targets = {}
    for position, call in enumerate(calls or []):
        target = clock_target(call.get("ts"), epoch, base)
        if target is None:
            continue
        target = max(int(target), previous + 1)
        key = call.get("i", position)
        targets[int(key)] = target
        previous = target
    return targets


def probe_clock_target(clock_by_index) -> int:
    """The one instant BOTH legs must evaluate post-replay probes at.

    The replay itself is pinned per entry, but the probe phase was not: the EVM
    leg answered from whatever pending block py-evm happened to hold while the
    AVM leg answered at LocalNet's last sealed block. Anything accruing with
    time then drifts. On Aave that was ~1.1 s of interest — a uniform 1.48e-7 of
    the window across every asset and every accruing view, which reads exactly
    like a ray-math miscompile rather than a harness artifact.

    One second past the last replayed entry: both clocks are forward-only, so
    the shared instant has to sit at or above the end of the schedule.
    """
    return (max(clock_by_index.values()) + 1) if clock_by_index else 0


def build_dep_tape_plans(case_dir: Path, skipped: set,
                         mapping20: dict | None = None,
                         calls: list | None = None):
    """Build selector-aware, transaction-bounded dependency answer plans.

    This includes calls whose selectors the generic stand-in also implements.
    A plan entry is served only when both its transaction range and selector
    match; otherwise the stand-in runs its native behavior. This lets an
    internal-call replay reproduce external state reads such as ERC-20
    ``balanceOf`` without letting an unavailable trace consume an answer
    belonging to a future transaction.
    """
    tp = case_dir / "dep_tape.json"
    if not tp.exists():
        return {}
    tapes = (load_json(tp) or {}).get("tapes") or {}
    if calls is None:
        calls = (load_json(case_dir / "calls.json") or {}).get("calls") or []
    hash_to_i = {}
    for c in calls:
        full = (c.get("hash") or "").lower()
        hash_to_i.setdefault(full, c["i"])
        hash_to_i.setdefault(full.split("#")[0], c["i"])

    plans = {}
    for addr, entries in tapes.items():
        answers, selectors, bounds = [], [], {}
        stalled_txn = None
        for entry in entries:
            h = (entry.get("hash") or "").lower()
            i = hash_to_i.get(h)
            if i is None or i in skipped:
                continue
            start, _ = bounds.setdefault(i, [len(answers), len(answers)])
            if stalled_txn == h:
                continue
            answer = entry.get("out")
            if answer is None:
                stalled_txn = h
                continue
            stalled_txn = None
            selector = (entry.get("sel") or "").removeprefix("0x")
            if len(selector) != 8:
                continue
            mapped = map_answer_words(bytes.fromhex(answer), mapping20 or {})
            answers.append(mapped)
            selectors.append(bytes.fromhex(selector))
            bounds[i] = [start, len(answers)]
        if answers:
            plans[addr.lower()] = {
                "answers": answers,
                "selectors": selectors,
                "bounds": {i: tuple(v) for i, v in bounds.items()
                           if v[0] != v[1]},
            }
    return plans


def map_answer_words(answer: bytes, mapping20: dict) -> bytes:
    """Translate HISTORICAL addresses inside a dependency answer into this
    leg's replay address space.

    Answers are ABI-encoded, hence word-aligned: scan whole 32-byte words; a
    word that has the EVM address shape (12 zero bytes + 20 content) whose
    content matches a registry entry is replaced by that leg's full 32-byte
    form (senders/creator become the leg's real account word — on the AVM that
    is the whole 32-byte account, NOT zero-prefixed). Anything else passes
    through untouched, including a trailing partial word. Word-aligned exact
    matching, not a heuristic: small integers can never match (no registered
    20-byte pattern), and offsets/lengths in dynamic payloads are small
    integers.
    """
    if not mapping20 or len(answer) < 32:
        return answer
    out = bytearray(answer)
    for i in range(0, len(answer) - 31, 32):
        w = answer[i:i + 32]
        if w[:12] == bytes(12):
            rep = mapping20.get(w[12:])
            if rep is not None:
                out[i:i + 32] = rep
    return bytes(out)


def tape_script_chunks(answers, selectors):
    """Selector/answer pairs grouped below the AVM app-argument budget."""
    if len(answers) != len(selectors):
        raise ValueError("dependency tape selector/answer length mismatch")
    out, chunk_words, chunk_lens, chunk_selectors = [], [], [], []
    for answer, selector in zip(answers, selectors):
        word_count = (len(answer) + 31) // 32 if answer else 0
        words = [answer[i * 32:(i + 1) * 32].ljust(32, b"\0")
                 for i in range(word_count)]
        # The loader has three dynamic-array arguments. Account for answer
        # words plus one length and selector word per entry; limiting only the
        # answer words can exceed the group argument budget on void answers.
        projected_words = len(chunk_words) + word_count
        projected_entries = len(chunk_lens) + 1
        if (chunk_lens
                and 32 * (projected_words + 2 * projected_entries) > 1_200):
            out.append((chunk_words, chunk_lens, chunk_selectors))
            chunk_words, chunk_lens, chunk_selectors = [], [], []
        chunk_words.extend(words)
        chunk_lens.append(len(answer))
        chunk_selectors.append(selector.ljust(32, b"\0"))
    if chunk_lens:
        out.append((chunk_words, chunk_lens, chunk_selectors))
    return out


def bytes32_mapping_key_candidates(calls, fns, keccak_fn, snapshots=None,
                                   getters=None):
    """bytes32 mapping keys evidenced by replay calldata.

    Besides literal bytes32 arguments, include bytes32 values observed through
    zero-argument getters. Public role/domain constants commonly key mappings
    without ever appearing in calldata. Also include the common Solidity helper
    ``keccak256(abi.encodePacked(uintN, bytes32))``.  CCTP's TokenMinter uses
    exactly that shape for ``remoteTokensToLocalTokens``: probing only the raw
    remote-token argument leaves every written entry invisible on both legs.

    ``keccak_fn`` is injected because the EVM venv and the system/AVM Python
    expose Keccak through different packages.  Results are ordered, lowercase
    32-byte hex strings without a ``0x`` prefix.
    """
    out = []

    def add(h):
        h = str(h or "").removeprefix("0x").lower()
        if len(h) == 64 and h not in out:
            out.append(h)

    for c in calls or []:
        args = c.get("args") or []
        for a in args:
            if isinstance(a, dict) and set(a) == {"__b__"}:
                add(a["__b__"])

        inputs = ((fns or {}).get(c.get("sig")) or {}).get("inputs") or []
        for i in range(min(len(args), len(inputs)) - 1):
            uint_t = str(inputs[i].get("type") or "")
            b32_t = str(inputs[i + 1].get("type") or "")
            m = re.fullmatch(r"uint(\d*)", uint_t)
            b = args[i + 1]
            if (not m or b32_t != "bytes32" or not isinstance(args[i], int)
                    or not isinstance(b, dict) or set(b) != {"__b__"}
                    or len(b["__b__"]) != 64):
                continue
            bits = int(m.group(1) or 256)
            n = args[i]
            if bits <= 0 or bits % 8 or not (0 <= n < (1 << bits)):
                continue
            packed = n.to_bytes(bits // 8, "big") + bytes.fromhex(b["__b__"])
            add(bytes(keccak_fn(packed)).hex())

    getter_outputs = {
        getter.get("sig"): getter.get("outputs") or []
        for getter in getters or []
    }
    for snapshot in (snapshots or {}).values():
        if not isinstance(snapshot, dict):
            continue
        for sig, values in snapshot.items():
            if not isinstance(values, list):
                continue
            for value, output in zip(values, getter_outputs.get(sig) or []):
                if output.get("type") == "bytes32" and isinstance(value, str):
                    add(value)
    return out


def call_without_consuming_tapes(call, cursors):
    """Run an EVM preflight without consuming Python-side answer tapes.

    ``eth_call`` rolls EVM state back, but the STATICCALL-compatible dependency
    interceptor keeps its cursors outside the EVM.  Restoring the complete map
    makes the subsequently mined transaction observe the same answer sequence,
    including when the preflight raises.
    """
    saved = cursors.copy()
    try:
        return call()
    finally:
        cursors.clear()
        cursors.update(saved)


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


def relax_pragma(src: str, *, pre08: bool = False) -> str:
    """Normalise pragmas so BOTH legs' compilers accept the same source.

    Two forms need rewriting, for opposite reasons:

    * exact-pinned `pragma solidity 0.8.N;` → `^0.8.N`, so the EVM leg's solc
      (a different 0.8.x) accepts it.
    * upper-bound-only `pragma solidity <0.9.0;` → `^0.8.0`. puya-sol's bundled
      solc is a PRERELEASE build (0.8.35-develop.…), and semver excludes
      prereleases from a plain `<` range, so these are rejected there while the
      EVM leg's release solc accepts them — 27 of Polymarket CTFExchange's 46
      files are written this way.
    With ``pre08=True``, any Solidity version range is deliberately replaced
    by ``^0.8.0``. This is an opt-in differential-corpus escape hatch for
    verified pre-0.8 contracts: both legs compile the identical transformed
    source, preserving oracle validity while explicitly giving up byte-level
    fidelity to the deployed compiler's unchecked-arithmetic semantics.
    """
    if pre08:
        src = re.sub(r"pragma solidity\s+[^;]+;",
                     "pragma solidity ^0.8.0;", src)
        # In 0.7, msg.sender was implicitly address payable; in 0.8 it is
        # address. The old OZ Context signature therefore stops the relaxed
        # source at type checking even though none of CCTP's call sites needs
        # the payable qualifier. Keep genuinely payable parameters untouched.
        src = re.sub(
            r"(function\s+_msgSender\s*\(\s*\)\s+internal\s+view\s+virtual\s+"
            r"returns\s*\(\s*)address\s+payable(\s*\))",
            r"\1address\2", src)
    src = re.sub(r"pragma solidity\s+(=)?0\.8\.(\d+)\s*;",
                 r"pragma solidity ^0.8.\2;", src)
    # ANY range with an explicit <0.9.0 upper bound (bare, or `>=0.8.x <0.9.0`)
    # excludes a prerelease compiler under semver — and so does the
    # TWO-component caret `^0.8;` (CoW's house style), while the ordinary
    # three-component `^0.8.N` is accepted. Normalise all three spellings.
    src = re.sub(r"pragma solidity\s*(>=\s*0\.[0-9.]+\s*)?<\s*0\.9\.0\s*;",
                 "pragma solidity ^0.8.0;", src)
    return re.sub(r"pragma solidity\s*\^\s*0\.8\s*;",
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
            or "too many inner transactions" in m
            # A single ApplicationArgs entry caps at 4096 bytes. A batch method
            # (World ID's registerIdentities carries 32416 bytes of identity
            # commitments) exceeds it in ONE argument, which the EVM has no
            # equivalent of. A transport ceiling, never a miscompile — see
            # `applicationargs length` in the AVM leg's revert text.
            or ("applicationargs" in m and "too long" in m))


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


def sender_marker(reg: dict, addr: str):
    """Marker for the SENDER role — never a `__dep__`.

    A dependency can also SEND (Raft's RToken is created and called by the
    PositionManager, which is simultaneously its ctor dep and the `from` of
    every internal call). The target role wants the locally deployed instance,
    but the sender role needs a SIGNABLE identity, so the sender view resolves
    creator → 'C' and any other dep-sender to its own registry symbol.
    """
    a = (addr or "").lower()
    if a == (reg.get("creator") or ""):
        return {"__addr__": "C"}
    if a in reg.get("senders", {}):
        return {"__addr__": reg["senders"][a]}
    m = marker_for(reg, a)
    if set(m) == {"__dep__"}:
        # Dep that sends but was never registered as one: fall back to the
        # arg-content identity so both legs still derive the SAME account.
        return {"__addr__": reg["args"].get(a, "?" + a[2:10])}
    return m


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
        values = ([v.get(c.get("name")) for c in comps]
                  if isinstance(v, Mapping) else list(v))
        return [canon_value(x, c.get("type", "uint256"), fold_addr,
                            c.get("components"))
                for x, c in zip(values, comps)]
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
        try:
            n = int(v)
        except (ValueError, TypeError):
            # a FOLDED address symbol ('C', '«7»') in a component whose ABI
            # metadata lacks a type and defaulted to uint256 — the symbol IS
            # the canonical value; both legs fold identically
            return v
        # A uint that IS a known identity (a stand-in's fallback answering its
        # own address, an address-valued word read through a uint getter)
        # renders leg-specifically (20-byte address vs escrow/app form) — fold
        # it to the shared symbol. Plain numbers miss and pass through; a
        # collision would need exact equality with a known 160+-bit identity.
        if fold_addr is not None and 2**64 <= n < 2**256:
            try:
                sym = fold_addr("0x%064x" % n)
                if isinstance(sym, str) and not sym.startswith("?"):
                    return sym
            except Exception:
                pass
        return n
    return v
