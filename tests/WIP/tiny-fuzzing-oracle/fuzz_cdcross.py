#!/usr/bin/env python3
"""CROSS-CONTRACT CALLDATA differential campaign — static-array args over the
external-call boundary.

fuzz_cd found the item-2 static-array bugs on the DIRECT-entry calldata read.
This stresses the OTHER side: when a Caller forwards static-array args
(bytes4[2], int16[2], uint8[3], …) to a Callee via `c.f(...)`, the CALLER must
ARC4-encode them and the Callee decodes them — a round-trip that could hide an
encoding mismatch the direct path never sees. Each fixture: a Callee that folds
EVERY element of its params into one uint256 observable (so any mis-encoded
element diverges) + a Caller that `new`s it and forwards. fuzz_state fuzzes the
caller inputs and diffs AVM vs live solc+py-evm.

Usage: python fuzz_cdcross.py [--contracts N] [--seed S] [--max-per-fn N]
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# (soltype, kind, fold-expr-template with {v})  — kind drives the arg value pool.
PARAMS = [
    ("int8", "int", "uint256(int256({v}))"),
    ("int16", "int", "uint256(int256({v}))"),
    ("int32", "int", "uint256(int256({v}))"),
    ("int128", "int", "uint256(int256({v}))"),
    ("uint8", "uint", "uint256({v})"),
    ("uint32", "uint", "uint256({v})"),
    ("uint256", "uint", "{v}"),
    ("bool", "bool", "({v} ? uint256(1) : uint256(0))"),
    ("bytes4", "bytesN", "uint256(uint32({v}))"),
    ("bytes32", "bytesN", "uint256({v})"),
    ("int16[2]", "arr", None), ("uint8[3]", "arr", None),
    ("uint256[2]", "arr", None), ("bytes4[2]", "arr", None),
    ("int128[2]", "arr", None),
]
ARR_FOLD = {  # static-array element fold
    "int16[2]": ("2", "uint256(int256({p}[{i}]))"),
    "uint8[3]": ("3", "uint256({p}[{i}])"),
    "uint256[2]": ("2", "{p}[{i}]"),
    "bytes4[2]": ("2", "uint256(uint32({p}[{i}]))"),
    "int128[2]": ("2", "uint256(int256({p}[{i}]))"),
}


def gen_fixture(tag, rng):
    n = rng.randrange(2, 5)
    chosen = rng.sample(PARAMS, n)
    names = [f"p{i}" for i in range(n)]

    cee_params, folds = [], []
    for (ty, kind, tmpl), nm in zip(chosen, names):
        loc = " calldata" if kind == "arr" else ""
        cee_params.append(f"{ty}{loc} {nm}")
        if kind == "arr":
            cnt, efold = ARR_FOLD[ty]
            for i in range(int(cnt)):
                folds.append(efold.format(p=nm, i=i))
        else:
            folds.append(tmpl.format(v=nm))
    fold_expr = " + ".join(folds) if folds else "uint256(0)"
    cee_sig = ", ".join(cee_params)

    # Caller forwards memory copies of the array args (calldata→external takes memory).
    cer_params = [d.replace(" calldata ", " memory ") for d in cee_params]
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_cdcross.py {tag} — cross-contract static-array calldata (generated).
contract Cee_{tag} {{
    function f({cee_sig}) external pure returns (uint256) {{
        unchecked {{ return {fold_expr}; }}
    }}
}}

contract Cer_{tag} {{
    Cee_{tag} c;
    constructor() {{ c = new Cee_{tag}(); }}
    function fwd({", ".join(cer_params)}) external returns (uint256) {{
        return c.f({", ".join(names)});
    }}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 60)
    seed0 = opt("--seed", 40000)
    max_per_fn = opt("--max-per-fn", 12)

    outdir = HERE / "out_cdcross"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        tag = f"x{seed}"
        fixture = outdir / f"cdcross_{seed}.sol"
        fixture.write_text(gen_fixture(tag, random.Random(seed)))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry=f"Cer_{tag}", max_per_fn=max_per_fn,
                                  budget_pool=6, harness=harness, quiet=True)
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            msg = str(e)[:200]
            if "solc" in msg.lower() or "compil" in msg.lower():
                skips += 1
                print(f"  ~ skip: {msg[:110]}")
            else:
                errors.append((seed, type(e).__name__ + ": " + msg))
                print(f"  ⚠️ runner error: {errors[-1][1]}")
            continue
        n_div = len(r["diverged"]) + len(r["revert_div"])
        if n_div or r["avm_errors"]:
            findings.append((seed, r))
            print(f"  ❌ seed {seed}: {len(r['diverged'])} value / "
                  f"{len(r['revert_div'])} revert / {len(r['avm_errors'])} AVM-err")
            for sig, args, exp, act in r["diverged"][:8]:
                print(f"     {sig}{tuple(args)}  evm={exp}  avm={act}")
            for sig, args, err in r["avm_errors"][:4]:
                print(f"     AVM-ERR {sig}{tuple(args)}: {err}")
        else:
            print(f"  ✅ {r['diffed']} calls clean")

    print("\n" + "=" * 60)
    print(f"CDCROSS CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
