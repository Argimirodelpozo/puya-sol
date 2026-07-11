#!/usr/bin/env python3
"""Differential fuzzing against a LIVE EVM — ABI-driven, any fixture, no hand-modeling.

Introspects a fixture's pure/view functions over integer types, synthesizes boundary
inputs per param, runs each on real solc + py-evm (evm_oracle.py in .evmvenv) AND on the
AVM (puya-sol → localnet, via the semantic-test framework), and diffs the decoded results
(canonicalized to the 256-bit pattern). This is the live-EVM successor to oracle.py: no
per-function hand-modeling — point it at any .sol with integer pure/view functions.

  python tests/WIP/tiny-fuzzing-oracle/fuzz_evm.py [fixture.sol] [--max-per-fn N]

Input generation per function: (a) sweep each param across its FULL boundary set with the
others held at a neutral baseline — catches single-param edges (e.g. shift==256); (b) a
capped cartesian of per-type key values — catches interactions (overflow, m==0, …).
Needs localnet up + the .evmvenv (see README).
"""
import itertools
import json
import re
import subprocess
import sys
from pathlib import Path

import algosdk.encoding as _algoenc   # driver venv only (the EVM oracle never imports this)

HERE = Path(__file__).resolve().parent

# Address bijection: a Solidity `address` maps to an AVM 32-byte account, but an EVM address
# is 20 bytes. Fuzz a small pool of logical SLOTS where the 32-byte content's low 20 bytes are
# the EVM address (slot 0 = the zero address); each side translates the marker to its own form
# (driver → algosdk base32, oracle → 20-byte hex) and address RETURNS canonicalise back to the
# 32-byte content hex so identity/mapping-key behaviour diffs cleanly (conversions like
# uint160(addr) are 20-vs-32-byte by design and are NOT generated).
_ADDR_SLOTS = 4


def _addr_content(i):
    return bytes(12) + int(i).to_bytes(20, "big")   # 32 bytes; low 20 = slot index
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "solidity-semantic-tests"))

from framework import Harness
from framework.localnet import LocalNet

EVM_PY = HERE / ".evmvenv" / "bin" / "python"
EVM_ORACLE = HERE / "evm_oracle.py"
DEFAULT_FIXTURE = HERE / "contracts" / "codec_probe.sol"
REVERT = "REVERT"
BITS = 1 << 256
_INT_RE = re.compile(r"(u?)int(\d+)$")


def _json_default(o):
    # bytes args (bytes/bytesN params) aren't JSON-serialisable for oracle transport;
    # tag them so the oracle can rebuild the exact bytes (the AVM side takes raw bytes).
    if isinstance(o, (bytes, bytearray)):
        return {"__b__": bytes(o).hex()}
    raise TypeError(f"not JSON-serialisable: {type(o).__name__}")


def _oracle(req):
    p = subprocess.run([str(EVM_PY), str(EVM_ORACLE)],
                       input=json.dumps(req, default=_json_default),
                       capture_output=True, text=True)
    if p.returncode != 0 or not p.stdout.strip():
        sys.exit("EVM oracle failed:\n" + (p.stderr[-2000:] or "(no stderr)"))
    return json.loads(p.stdout)


def _midrange(bits):
    """Scattered mid-range bit patterns for a `bits`-wide unsigned value: alternating (0x55../0xAA..),
    thirds, and a deterministic scatter. The curated edge set is all-zeros/all-ones/single-bit; these
    exercise INTERIOR bit patterns where mask / sub-word-codec bugs hide (a mask that drops the wrong
    bits looks fine at 0 and max but corrupts 0x5555..)."""
    mx = (1 << bits) - 1
    return {
        int("01" * ((bits + 1) // 2), 2) & mx,     # 0x55..55 alternating
        int("10" * ((bits + 1) // 2), 2) & mx,     # 0xAA..AA alternating (high bit set)
        mx // 3,
        (2 * mx) // 3,
        (mx * 7 // 11) & mx,                        # deterministic scatter
    }


def boundaries(sol_type):
    """Full boundary set for an intN/uintN ABI type, or None if not a fuzzable scalar. Includes the
    curated edges PLUS scattered mid-range bit patterns (_midrange) — edges alone miss interior-bit bugs."""
    m = _INT_RE.match(sol_type)
    if not m:
        return None
    signed, bits = (m.group(1) == ""), int(m.group(2))
    if signed:
        hi, lo = (1 << (bits - 1)) - 1, -(1 << (bits - 1))
        vals = {0, 1, -1, 2, -2, hi, lo, hi - 1, lo + 1}
        for b in (8, 24, 128):                     # sub-width sign edges
            if b < bits:
                h = 1 << (b - 1)
                vals.update({h - 1, h, -h, -h - 1})
        for u in _midrange(bits):                  # mid-range patterns, two's-complement reinterpreted
            vals.add(u if u <= hi else u - (1 << bits))   # high-bit-set patterns become negative
        return sorted(v for v in vals if lo <= v <= hi)
    mx = (1 << bits) - 1
    vals = {0, 1, 2, mx, mx - 1, mx >> 1}
    for b in (8, 24, 128, 256):                    # sub-width edges
        if b <= bits:
            vals.add((1 << b) - 1)
        if b < bits:
            vals.update({1 << b, (1 << b) + 5})
    if bits >= 9:
        vals.update({255, 256, 257, 300})          # shift-saturation boundary
    vals.update(_midrange(bits))                    # mid-range interior-bit patterns
    return sorted(v for v in vals if 0 <= v <= mx)


def _keys(sol_type):
    """A few representative values per type, for the interaction cartesian."""
    m = _INT_RE.match(sol_type)
    signed, bits = (m.group(1) == ""), int(m.group(2))
    if signed:
        return sorted({0, 1, -1, (1 << (bits - 1)) - 1, -(1 << (bits - 1))})
    mx = (1 << bits) - 1
    return sorted({0, 1, mx, (256 if bits >= 9 else mx)})


_ARR_RE = re.compile(r"^(.*)\[(\d*)\]$")
_BYTESN_RE = re.compile(r"^bytes(\d+)$")


def _bytesn_vals(n):
    """Boundary N-byte values for a fixed bytesN: zero, all-ones, high-bit-only, low-bit-only,
    and an ascending pattern (catches byte-order / packing bugs)."""
    vals = [bytes(n), bytes([0xFF] * n), bytes([0x80] + [0] * (n - 1)),
            bytes([0] * (n - 1) + [1]), bytes((i + 1) & 0xFF for i in range(n))]
    seen, out = set(), []
    for v in vals:
        if v not in seen:
            seen.add(v); out.append(v)
    return out


def candidates(inp):
    """Representative VALUES for an ABI input entry, simplest first, or None if not cleanly
    fuzzable. Recurses through arrays (T[] / T[N]) and tuples (structs); leaves are the clean
    computational set intN/uintN/bool + bytesN/bytes/string (the byte content round-trips through
    both ABIs, so the decoded VALUE diffs even though the wire encoding differs). address → None."""
    t = inp["type"]
    m = _ARR_RE.match(t)
    if m:
        elem = {**inp, "type": m.group(1)}
        ev = candidates(elem)
        if ev is None:
            return None
        if m.group(2) == "":                              # dynamic T[]
            out = [[], [ev[0]]]
            if len(ev) > 1:
                out.append([ev[i] for i in range(min(4, len(ev)))])   # several distinct
                out.append([ev[-1], ev[0]])                            # boundary + simple
            return out
        n = int(m.group(2))                               # fixed T[n]
        return [[ev[0]] * n, [ev[k % len(ev)] for k in range(n)]]
    if t == "tuple":
        cc = [candidates(c) for c in inp.get("components", [])]
        if any(c is None for c in cc):
            return None
        base = [c[0] for c in cc]
        out = [list(base)]
        for i, ci in enumerate(cc):                       # vary one component at a time
            for v in ci[1:3]:
                row = list(base); row[i] = v; out.append(row)
        return out
    b = boundaries(t)
    if b is not None:
        return b
    if t == "bool":
        return [False, True]
    mb = _BYTESN_RE.match(t)                                # fixed bytes1..bytes32
    if mb:
        return _bytesn_vals(int(mb.group(1)))
    if t == "bytes":                                       # dynamic bytes: empty, edges, longer-than-word
        return [b"", b"\x00", b"\xff", b"abc", bytes(range(32)), bytes([0xAB]) * 40]
    if t == "string":                                     # ascii + multibyte (UTF-8) + empty
        return ["", "a", "abc", "hello world", "é中\U0001f600"]
    if t == "address":                                    # logical slots (slot 0 = zero address)
        return [{"__addr__": i} for i in range(_ADDR_SLOTS)]
    return None                                            # unmapped (e.g. fixed-point) → skip fn


def _args_to_algo(o):
    """Driver side: translate {"__addr__": i} markers → algosdk base32 addresses (recursive)."""
    if isinstance(o, dict) and set(o) == {"__addr__"}:
        return _algoenc.encode_address(_addr_content(o["__addr__"]))
    if isinstance(o, list):
        return [_args_to_algo(x) for x in o]
    return o


def _canon_addr(value, out_type):
    """Canonicalise an address-typed RETURN to its 32-byte content hex, from either the EVM hex
    form (web3) or the AVM base32 form (algosdk). Recurses address[] / leaves non-address as-is."""
    if out_type == "address":
        if isinstance(value, str) and value.startswith(("0x", "0X")):
            return "0x" + value[2:].rjust(64, "0").lower()      # 20-byte EVM hex → 32-byte
        if isinstance(value, str):
            return "0x" + _algoenc.decode_address(value).hex()  # AVM base32 → 32-byte
        return value
    m = _ARR_RE.match(out_type or "")
    if m and m.group(1) == "address" and isinstance(value, list):
        return [_canon_addr(x, "address") for x in value]
    return value


def _dedup_add(rows, seen, row):
    # composite rows hold lists (not hashable) and may hold bytes (not JSON-serialisable);
    # repr is deterministic for the list/int/bool/bytes/str values we generate.
    key = repr(row)
    if key not in seen:
        seen.add(key)
        rows.append(row)


def gen_rows(ins, max_per_fn):
    """Per-param boundary sweep + capped interaction cartesian over candidate VALUES (handles
    scalars, arrays, tuples). Returns the arg-rows, or None if any param is non-fuzzable."""
    per = [candidates(i) for i in ins]
    if any(p is None for p in per):
        return None
    base = [p[0] for p in per]                       # simplest candidate each (0 / [] / base-tuple)
    rows, seen = [], set()
    for i, p in enumerate(per):                      # (a) per-param full sweep
        for v in p:
            row = [x for x in base]; row[i] = v
            _dedup_add(rows, seen, row)
    for combo in itertools.product(*[p[:2] for p in per]):  # (b) interactions (first 2 each)
        if len(rows) >= max_per_fn:
            break
        _dedup_add(rows, seen, list(combo))
    return rows[:max_per_fn]


def gen_calls(fns, max_per_fn):
    calls, skipped = [], []
    for fn in fns:
        sig = fn["sig"]
        if not fn["outputs"]:
            skipped.append((sig, "no return value to diff"))
            continue
        # No pure/view filter: state-changing self-contained probes run via .call()/simulate
        # (writes visible within the call, never persisted), so they diff cleanly too.
        if not fn["inputs"]:                # zero-arg designed probe → one call
            calls.append((sig, []))
            continue
        rows = gen_rows(fn["inputs"], max_per_fn)
        if rows is None:
            skipped.append((sig, "non-fuzzable params (address/bytes/string)"))
            continue
        for row in rows:
            calls.append((sig, row))
    return calls, skipped


def canon(v):
    if isinstance(v, (list, tuple)):                 # arrays/structs/tuples (web3→tuple, algosdk→list)
        return [canon(x) for x in v]
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (bytes, bytearray)):            # bytes/bytesN → list of byte-values (algosdk shape)
        return list(v)
    return v % BITS if isinstance(v, int) else v


def _apply_addr_canon(value, outs):
    """Canonicalise address(es) in a return per the function's output types (no-op if none)."""
    if value is REVERT or not outs:
        return value
    if len(outs) == 1:
        return _canon_addr(value, outs[0])
    if isinstance(value, (list, tuple)):
        return [_canon_addr(v, t) for v, t in zip(value, outs)]
    return value


def avm_call(h, app, sig, args):
    r = h.call(app, sig, *_args_to_algo(args), expect_revert=True)
    return REVERT if r.reverted else r.abi_return


def _fmt1(v):
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(_fmt1(x) for x in v) + "]"
    if isinstance(v, (bytes, bytearray)):
        h = bytes(v).hex()
        return "0x" + (h if len(h) <= 24 else h[:20] + "…")
    if isinstance(v, int) and abs(v) > (1 << 40):
        return hex(v) if v >= 0 else "-" + hex(-v)
    return str(v)


def _fmt(args):
    return "(" + ", ".join(_fmt1(a) for a in args) + ")"


def main():
    argv = list(sys.argv[1:])
    max_per_fn = 80
    if "--max-per-fn" in argv:
        i = argv.index("--max-per-fn")
        max_per_fn = int(argv[i + 1])
        del argv[i:i + 2]
    fixture = Path(argv[0]).resolve() if argv else DEFAULT_FIXTURE

    base = {"fixture": str(fixture), "solc_version": "0.8.26", "evm_version": "paris"}
    print(f"[introspect] {fixture.name}…")
    info = _oracle({**base, "introspect": True})
    calls, skipped = gen_calls(info["functions"], max_per_fn)
    print(f"  contract {info['contract']}: {len(info['functions'])} fns → {len(calls)} "
          f"calls over {len({s for s, _ in calls})} fuzzable fns")
    for sig, why in skipped:
        print(f"  · skip {sig}  ({why})")
    if not calls:
        sys.exit("no fuzzable pure/view integer functions found")

    outs_by_sig = {f["sig"]: f["outputs"] for f in info["functions"]}

    print(f"[EVM] running {len(calls)} calls…")
    evm = _oracle({**base, "calls": [{"sig": s, "args": a} for s, a in calls]})["results"]

    ln = LocalNet()
    h = Harness(ln, HERE / "out_evm")
    print("[AVM] compiling + deploying…")
    app = h.compile_and_deploy(fixture)

    total = 0
    diverged, avm_errors, evm_skips = [], [], []
    for (sig, args), er in zip(calls, evm):
        total += 1
        if er.get("ok"):
            expected = er["value"]
        elif er.get("revert"):
            expected = REVERT
        else:
            evm_skips.append((sig, args, er.get("err", "?")))
            continue
        try:
            actual = avm_call(h, app, sig, args)
        except Exception as e:
            avm_errors.append((sig, args, type(e).__name__ + ": " + str(e)[:140]))
            continue
        outs = outs_by_sig.get(sig, [])
        exp_c = _apply_addr_canon(expected, outs)       # address returns → 32-byte content hex
        act_c = _apply_addr_canon(actual, outs)
        if canon(act_c) != canon(exp_c):
            diverged.append((sig, args, exp_c, act_c))

    diffed = total - len(avm_errors) - len(evm_skips)
    print(f"\n=== {diffed} calls diffed (AVM vs live EVM) ===")
    if evm_skips:
        print(f"\n⚠️  {len(evm_skips)} EVM-side skips (arg encoding etc.):")
        for sig, args, err in evm_skips[:10]:
            print(f"   {sig}{_fmt(args)}: {err}")
    if avm_errors:
        print(f"\n⚠️  {len(avm_errors)} AVM call errors (could not execute — coverage gap / harness):")
        for sig, args, err in avm_errors[:20]:
            print(f"   {sig}{_fmt(args)}: {err}")
    if diverged:
        print(f"\n❌ {len(diverged)} DIVERGENCE(S):")
        for sig, args, exp, act in diverged[:80]:
            print(f"   {sig}{_fmt(args)}  evm={_fmt1(exp)}  avm={_fmt1(act)}")
    else:
        print("\n✅ no divergences — AVM matches a live solc+EVM on every executed input")
    return 1 if diverged else 0


if __name__ == "__main__":
    sys.exit(main())
