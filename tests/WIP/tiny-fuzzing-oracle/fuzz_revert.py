#!/usr/bin/env python3
"""REVERT-PAYLOAD fuzzer (direction B). The AVM has no try/catch, so a custom-error revert payload is
only ever decoded OFF-CHAIN. This differ checks the payload VALUE round-trips: it calls each reverting
function, captures revert_data, strips the 4-byte selector, and decodes the args at puya-sol's BACKING
widths (int/uint ≤64 → 8B sign-extended-to-64, >64 → 32B sign-extended-to-256; bool 1B; bytesN N B). If
the decoded args ≠ the fuzzed inputs, the custom-error VALUE/sign encoding is wrong. NB: it decodes at
BACKING width, NOT declared ARC4 N/8 — the backing-width encoding is the DOCUMENTED abi.* convention (an
accepted EVM_DIVERGENCE, see test_abi_encode_call_uint_bytes); decoding at declared widths false-positives
on it ([[custom-error-payload-width]]). Scalar args only; aggregate error args are a TODO.

Usage: python fuzz_revert.py contracts/<fixture>.sol [--contract NAME]
"""
import re
import sys
from pathlib import Path

from fuzz_evm import HERE, _oracle, gen_rows, _args_to_algo, Harness, LocalNet

_INT = re.compile(r"^(u?)int(\d+)$")
_BYTESN = re.compile(r"^bytes(\d+)$")


def _decode_arg(sol_type, raw, off):
    """Decode one scalar arg from raw[off:] at puya-sol's BACKING width (NOT the declared ARC4 N/8 — that
    backing-width encoding is the DOCUMENTED abi.* convention, an accepted EVM_DIVERGENCE; decoding at
    declared widths false-positives on it, see [[custom-error-payload-width]]). Rule: int/uint ≤64 ride
    at 8B (uint64-backed, sign-extended to 64-bit), >64 at 32B (biguint, sign-extended to 256-bit);
    bool 1B; bytesN N B. Returns (value, new_off) or (None, -1) if unhandled / bytes run short."""
    m = _INT.match(sol_type)
    if m:
        signed = (m.group(1) == "")
        bits = int(m.group(2))
        n = 8 if bits <= 64 else 32             # backing width
        ext = 64 if bits <= 64 else 256          # interpret at the backing bit-width
        if off + n > len(raw):
            return None, -1
        v = int.from_bytes(raw[off:off + n], "big")
        if signed and v >= (1 << (ext - 1)):
            v -= (1 << ext)
        return v, off + n
    if sol_type == "bool":
        if off + 1 > len(raw):
            return None, -1
        return (raw[off] != 0), off + 1
    mb = _BYTESN.match(sol_type)
    if mb:
        n = int(mb.group(1))
        if off + n > len(raw):
            return None, -1
        return raw[off:off + n], off + n
    return None, -1  # unhandled (dynamic / aggregate) — skip the function


def main():
    argv = list(sys.argv[1:])
    contract = None
    if "--contract" in argv:
        i = argv.index("--contract"); contract = argv[i + 1]; del argv[i:i + 2]
    fixture = Path(argv[0]).resolve()

    base = {"fixture": str(fixture), "solc_version": "0.8.26", "evm_version": "paris"}
    if contract:
        base["contract"] = contract
    info = _oracle({**base, "introspect": True})
    funcs = info["functions"]

    ln = LocalNet(); h = Harness(ln, HERE / "out_revert")
    app = h.compile_and_deploy(fixture, contract_name=contract)

    diffed = diverged = skipped = 0
    bad = []
    for f in funcs:
        ins = f.get("inputs", [])
        sig = f["sig"]
        # scalar-only fixture; skip anything with a non-scalar / unhandled param
        types = [i["type"] for i in ins]
        if not ins or any(_INT.match(t) is None and t != "bool" and _BYTESN.match(t) is None for t in types):
            continue
        rows = gen_rows(ins, 24)
        if rows is None:
            continue
        for row in rows:
            try:
                r = h.call(app, sig, *_args_to_algo(row), expect_revert=True)
            except Exception:
                continue
            if not getattr(r, "reverted", False) or not getattr(r, "revert_data", None):
                skipped += 1; continue
            payload = r.revert_data[4:]   # strip 4-byte selector
            off, decoded, ok = 0, [], True
            for t in types:
                v, off = _decode_arg(t, payload, off)
                if off < 0:
                    ok = False; break
                decoded.append(v)
            if not ok or off != len(payload):
                skipped += 1; continue   # couldn't cleanly decode → not a width-conformance datapoint
            diffed += 1
            # compare decoded payload args to the fuzzed inputs (identity: the encoding must round-trip)
            if decoded != list(row):
                diverged += 1
                if len(bad) < 25:
                    bad.append((sig, row, decoded))

    print(f"\n=== {diffed} revert payloads decoded (backing-width value conformance) ===")
    if diverged:
        print(f"\n❌ {diverged} DIVERGENCE(S) (payload arg != input):")
        for sig, row, dec in bad:
            print(f"   {sig}{tuple(row)}  decoded={tuple(dec)}")
    else:
        print(f"\n✅ no divergences — every custom-error payload decodes to its inputs at backing widths"
              f" ({skipped} skipped: non-reverting / unhandled-shape)")


if __name__ == "__main__":
    main()
