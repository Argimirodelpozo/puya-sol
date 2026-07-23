#!/usr/bin/env python3
"""CALLDATA-LAYOUT differential campaign — targets the item-2 rewrite (v467).

The synthetic `__cd_blob` and the constant-offset calldata map are the
EVM-32-byte-word views over ARC4-packed params; item 2 rederived their layout
+ value widening from solc types. This driver stresses exactly that surface:
generated contracts mix the risky param types (signed sub-word, bytesN, static
aggregates, sub-word-element dynamic arrays, dynamic bytes) and read them back
through inline assembly —
  - calldataload(<const>) at ABI head offsets computed HERE from the standard
    ABI spec (independent of item-2's C++ walk — a layout bug diverges),
  - <dyn>.offset / <dyn>.length,
  - calldatasize(),
  - keccak256(<dyn>.offset, <dyn>.length) over a calldata region.
Returns are bytes32 (raw word) / uint so every read is an EXACT differential
against live solc + py-evm via run_stateful_diff (values + revert payloads).

Usage: python fuzz_cd.py [--contracts N] [--seed S] [--max-per-fn N]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# (soltype, kind, head_bytes, is_value_readable_at_offset)
# head_bytes = ABI head size (statics inline their full encoded size; dynamics
# are a 32-byte pointer). Standard ABI — NOT read from item-2's code.
PARAMS = [
    ("int8", "value", 32), ("int16", "value", 32), ("int24", "value", 32),
    ("int32", "value", 32), ("int128", "value", 32),
    ("uint8", "value", 32), ("uint16", "value", 32), ("uint256", "value", 32),
    ("bool", "value", 32), ("bytes4", "value", 32), ("bytes32", "value", 32),
    ("address", "value", 32),
    ("uint8[3]", "static", 96), ("uint256[2]", "static", 64),
    ("int16[2]", "static", 64), ("bytes4[2]", "static", 64),
    ("uint8[]", "dyn", 32), ("int16[]", "dyn", 32),
    ("uint256[]", "dyn", 32), ("bytes", "dyn", 32),
]

STATIC_ELEMS = {  # static array → (count, per-elem head bytes)
    "uint8[3]": (3, 32), "uint256[2]": (2, 32),
    "int16[2]": (2, 32), "bytes4[2]": (2, 32),
}


def gen_contract(seed):
    rng = random.Random(seed)
    n = rng.randrange(2, 5)
    chosen = rng.sample(PARAMS, min(n, len(PARAMS)))
    # ensure at least one dynamic sometimes (tail layout), at least one static
    names = [f"p{i}" for i in range(len(chosen))]
    decls = []
    for (ty, kind, _), nm in zip(chosen, names):
        loc = " calldata" if kind != "value" else ""
        decls.append(f"{ty}{loc} {nm}")
    param_list = ", ".join(decls)
    # sink so no param is "unused" (solc warns, harmless, but keep clean)
    sink = "".join(f" {nm};" for nm in names)

    # ABI head offsets (start at 4 for the selector).
    offs, off = [], 4
    for (ty, kind, hb) in chosen:
        offs.append(off)
        off += hb

    fns = []
    fi = 0

    def emit(sig_body):
        nonlocal fi
        fns.append(sig_body)
        fi += 1

    # calldatasize — the whole layout's total length.
    emit(f"""
    function cs({param_list}) external pure returns (uint256 r) {{
        assembly {{ r := calldatasize() }}{sink}
    }}""")

    for (ty, kind, hb), nm, o in zip(chosen, names, offs):
        if kind == "value":
            emit(f"""
    function hw{fi}({param_list}) external pure returns (bytes32 r) {{
        assembly {{ r := calldataload({o}) }}{sink}
    }}""")
        elif kind == "static":
            cnt, esz = STATIC_ELEMS[ty]
            for j in range(cnt):
                emit(f"""
    function hw{fi}({param_list}) external pure returns (bytes32 r) {{
        assembly {{ r := calldataload({o + j * esz}) }}{sink}
    }}""")
        else:  # dyn: length, first two element words, keccak over the region
            emit(f"""
    function dl{fi}({param_list}) external pure returns (uint256 r) {{
        assembly {{ r := {nm}.length }}{sink}
    }}""")
            emit(f"""
    function de{fi}({param_list}) external pure returns (bytes32 a, bytes32 b) {{
        assembly {{ a := calldataload({nm}.offset)
                   b := calldataload(add({nm}.offset, 32)) }}{sink}
    }}""")
            emit(f"""
    function kc{fi}({param_list}) external pure returns (bytes32 r) {{
        assembly {{ r := keccak256({nm}.offset, {nm}.length) }}{sink}
    }}""")

    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_cd.py seed={seed} — calldata-layout differential fixture (generated).
contract CdFuzz {{
{''.join(fns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 60)
    seed0 = opt("--seed", 70000)
    max_per_fn = opt("--max-per-fn", 8)

    outdir = HERE / "out_cd"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, compile_fails = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"cd_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="CdFuzz", max_per_fn=max_per_fn,
                                  harness=harness, quiet=True)
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            msg = str(e)[:200]
            if "solc" in msg.lower() or "compil" in msg.lower():
                compile_fails += 1
                print(f"  ~ compile/oracle skip: {msg[:120]}")
            else:
                errors.append((seed, type(e).__name__ + ": " + msg))
                print(f"  ⚠️ runner error: {errors[-1][1]}")
            continue
        n_div = len(r["diverged"]) + len(r["revert_div"])
        if n_div or r["avm_errors"]:
            findings.append((seed, r))
            print(f"  ❌ seed {seed}: {len(r['diverged'])} value / "
                  f"{len(r['revert_div'])} revert-payload divergences, "
                  f"{len(r['avm_errors'])} AVM errors")
            for sig, args, exp, act in r["diverged"][:8]:
                print(f"     {sig}{tuple(args)}  evm={exp}  avm={act}")
            for sig, args, err in r["avm_errors"][:4]:
                print(f"     AVM-ERR {sig}{tuple(args)}: {err}")
        else:
            print(f"  ✅ {r['diffed']} calls clean")

    print("\n" + "=" * 60)
    print(f"CD CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {compile_fails} compile/oracle skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/"
              f"{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
