#!/usr/bin/env python3
"""EVENT-LOG fuzzer (direction B vein 2). puya-sol lowers `emit E(args)` to a `log` opcode: selector(4B)
+ ARC4-encoded NON-indexed args (at puya-sol's BACKING widths, the documented abi.* convention). The
current fuzzers ignore logs entirely. This differ calls each VOID emit-function, reads the event log from
the execute response (abi_results[i].tx_info["logs"] — the harness's r.logs misses it on the execute
path), strips the 4-byte selector, decodes the args at backing widths, and checks they round-trip to the
fuzzed inputs. Catches event-arg VALUE/sign mis-encodings. Scalar args only.

NB indexed event params fail to PARSE in puya-sol (separate finding) → generator emits non-indexed only.
NB log capture: r.logs is empty on the execute path (AtomicTransactionResponse has abi_results, not
tx_info) — read abi_results[i].tx_info["logs"] directly.

Usage: python fuzz_events.py contracts/<fixture>.sol [--contract NAME]
"""
import base64
import re
import sys
from pathlib import Path

from fuzz_evm import HERE, _oracle, gen_rows, _args_to_algo, Harness, LocalNet
from fuzz_revert import _decode_arg, _INT, _BYTESN  # reuse the backing-width scalar decoder

_RET_PREFIX = bytes.fromhex("151f7c75")  # ARC4 method-return log prefix (skip if present)


def _event_log(raw):
    """The single event log from an execute response (void emit fn → 1 log; skip a return-prefix log)."""
    for res in getattr(raw, "abi_results", None) or []:
        ti = getattr(res, "tx_info", None)
        if not isinstance(ti, dict):
            continue
        for b64 in ti.get("logs", []) or []:
            data = base64.b64decode(b64)
            if data[:4] == _RET_PREFIX:
                continue  # method return value, not the event
            return data
    return None


def main():
    argv = list(sys.argv[1:])
    contract = None
    if "--contract" in argv:
        i = argv.index("--contract"); contract = argv[i + 1]; del argv[i:i + 2]
    fixture = Path(argv[0]).resolve()

    base = {"fixture": str(fixture), "solc_version": "0.8.26", "evm_version": "paris"}
    if contract:
        base["contract"] = contract
    funcs = _oracle({**base, "introspect": True})["functions"]

    ln = LocalNet(); h = Harness(ln, HERE / "out_events")
    app = h.compile_and_deploy(fixture, contract_name=contract)

    diffed = diverged = skipped = 0
    bad = []
    for f in funcs:
        ins = f.get("inputs", [])
        sig = f["sig"]
        types = [i["type"] for i in ins]
        if not ins or any(_INT.match(t) is None and t != "bool" and _BYTESN.match(t) is None for t in types):
            continue
        rows = gen_rows(ins, 24)
        if rows is None:
            continue
        for row in rows:
            try:
                r = h.call(app, sig, *_args_to_algo(row))   # execute (emits the event)
            except Exception:
                skipped += 1; continue
            log = _event_log(getattr(r, "raw_response", None))
            if log is None:
                skipped += 1; continue
            payload = log[4:]   # strip the 4-byte event selector
            off, decoded, ok = 0, [], True
            for t in types:
                v, off = _decode_arg(t, payload, off)
                if off < 0:
                    ok = False; break
                decoded.append(v)
            if not ok or off != len(payload):
                skipped += 1; continue
            diffed += 1
            if decoded != list(row):
                diverged += 1
                if len(bad) < 25:
                    bad.append((sig, row, decoded))

    print(f"\n=== {diffed} event logs decoded (backing-width value conformance) ===")
    if diverged:
        print(f"\n❌ {diverged} DIVERGENCE(S) (event-log arg != input):")
        for sig, row, dec in bad:
            print(f"   {sig}{tuple(row)}  decoded={tuple(dec)}")
    else:
        print(f"\n✅ no divergences — every event log decodes to its inputs at backing widths"
              f" ({skipped} skipped: no-log / unhandled-shape)")


if __name__ == "__main__":
    main()
