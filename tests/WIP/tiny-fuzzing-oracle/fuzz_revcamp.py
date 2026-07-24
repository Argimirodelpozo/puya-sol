#!/usr/bin/env python3
"""REVERT-PAYLOAD campaign — generated custom-error reverts, round-tripped.

The standalone fuzz_revert differs ONE hand-written fixture; there's no campaign.
This generates random contracts with custom errors carrying scalar args
(signed/unsigned ints of many widths, bool, bytesN) and functions that revert
with them, then reuses fuzz_revert's decoder: strip the 4-byte selector,
ARC4/backing-width-decode the args, and check they ROUND-TRIP the fuzzed inputs
(sidesteps the sha512_256-vs-keccak selector divergence — the AVM payload must
decode back to what was passed). Scalar args only (aggregate custom-error args
use a mixed ARC4-element / backing-width encoding — a separate decoder TODO).

Usage: python fuzz_revcamp.py [--contracts N] [--seed S] [--max-per-fn N]
"""
import random
import sys
from pathlib import Path

from fuzz_evm import HERE, _oracle, gen_rows, _args_to_algo, Harness, LocalNet
from fuzz_revert import _decode_arg, _INT, _BYTESN

SCALARS = ["uint8", "int8", "uint16", "int16", "uint24", "int24", "uint32",
           "int40", "uint64", "int64", "uint128", "int128", "uint256", "int256",
           "bool", "bytes1", "bytes4", "bytes8", "bytes16", "bytes32"]


def gen_contract(seed):
    rng = random.Random(seed)
    errs, fns = [], []
    for i in range(rng.randrange(4, 9)):
        n = rng.randrange(1, 4)
        types = [rng.choice(SCALARS) for _ in range(n)]
        params = ", ".join(f"{t} v{j}" for j, t in enumerate(types))
        args = ", ".join(f"v{j}" for j in range(n))
        errs.append(f"    error E{i}({', '.join(types)});")
        fns.append(f"""
    function r{i}({params}) external pure {{ revert E{i}({args}); }}""")
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_revcamp.py seed={seed} — revert-payload differential fixture (generated).
contract RevFuzz {{
{chr(10).join(errs)}
{''.join(fns)}
}}
"""


def check_fixture(h, fixture):
    """Round-trip every reverting fn's custom-error payload. Returns
    (diffed, diverged_list, skipped)."""
    base = {"fixture": str(fixture), "solc_version": "0.8.26", "evm_version": "paris",
            "contract": "RevFuzz"}
    info = _oracle({**base, "introspect": True})
    app = h.compile_and_deploy(fixture, contract_name="RevFuzz")
    diffed, skipped, diverged = 0, 0, []
    for f in info["functions"]:
        ins = f.get("inputs", [])
        types = [i["type"] for i in ins]
        if not ins or any(_INT.match(t) is None and t != "bool" and _BYTESN.match(t) is None
                          for t in types):
            continue
        rows = gen_rows(ins, 20)
        if rows is None:
            continue
        for row in rows:
            try:
                r = h.call(app, f["sig"], *_args_to_algo(row), expect_revert=True)
            except Exception:
                continue
            if not getattr(r, "reverted", False) or not getattr(r, "revert_data", None):
                skipped += 1; continue
            payload = r.revert_data[4:]
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
                diverged.append((f["sig"], row, decoded))
    return diffed, diverged, skipped


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 80)
    seed0 = opt("--seed", 3000)

    outdir = HERE / "out_revcamp"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, total_diffed = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"rev_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            diffed, diverged, skipped = check_fixture(harness, fixture)
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            errors.append((seed, type(e).__name__ + ": " + str(e)[:160]))
            print(f"  ⚠️ runner error: {errors[-1][1]}")
            continue
        total_diffed += diffed
        if diverged:
            findings.append((seed, diverged))
            print(f"  ❌ seed {seed}: {len(diverged)} payload divergence(s) / {diffed} diffed")
            for sig, row, dec in diverged[:8]:
                print(f"     {sig}{tuple(row)}  decoded={tuple(dec)}")
        else:
            print(f"  ✅ {diffed} payloads round-trip ({skipped} skipped)")

    print("\n" + "=" * 60)
    print(f"REVCAMP DONE: {n_contracts} contracts, {total_diffed} payloads diffed, "
          f"{len(findings)} with findings, {len(errors)} runner errors")
    for seed, dv in findings:
        print(f"  seed {seed}: {len(dv)} divergences")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
