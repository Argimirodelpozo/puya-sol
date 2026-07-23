#!/usr/bin/env python3
"""COMBINATION differential campaign — packed storage × sequencing × side effects.

The single-surface campaigns each passed in isolation; bugs hide at their
INTERSECTION. This one composes:
  - PACKED storage (sub-word fields sharing a slot; read = decode/sign-extend,
    write = read-modify-write that must not clobber neighbours),
  - SEQUENCING (side-effecting calls inside expressions — legacy solc evaluates
    a binop's RIGHT operand first, assignments RHS-first; the OperandPlan seam),
  - SIDE EFFECTS (internal calls / ++/-- / compound-assign that MUTATE a packed
    field mid-expression while a sibling reads a neighbour field).
Generated contracts mix packed struct + bare packed state vars, internal
helpers that mutate a packed field and return its widened value, and public
mutators that compose those helpers with neighbour reads under mixed evaluation
order — then getters. run_stateful_diff interleaves + diffs persisted state vs
live solc+py-evm. `unchecked` isolates value/ordering/packing from overflow.

Usage: python fuzz_combo.py [--contracts N] [--seed S] [--max-per-fn N]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# (soltype, widen-to-uint256 template {v})
PACKED = [
    ("uint8", "uint256({v})"), ("int8", "uint256(uint8({v}))"),
    ("uint16", "uint256({v})"), ("int16", "uint256(uint16({v}))"),
    ("uint24", "uint256({v})"), ("int24", "uint256(uint24({v}))"),
    ("uint32", "uint256({v})"), ("bool", "({v} ? uint256(1) : uint256(0))"),
    ("bytes4", "uint256(uint32({v}))"),
]
NUMERIC = {"uint8", "int8", "uint16", "int16", "uint24", "int24", "uint32"}


def gen_contract(seed):
    rng = random.Random(seed)
    # packed struct fields + bare packed state vars
    sfields = [(f"a{i}", *rng.choice(PACKED)) for i in range(rng.randrange(3, 6))]
    bvars = [(f"g{i}", *rng.choice(PACKED)) for i in range(rng.randrange(2, 4))]
    all_num = ([("st." + n, ty, w) for n, ty, w in sfields if ty in NUMERIC]
               + [(n, ty, w) for n, ty, w in bvars if ty in NUMERIC])
    all_read = ([("st." + n, ty, w) for n, ty, w in sfields]
                + [(n, ty, w) for n, ty, w in bvars])

    smembers = "".join(f" {ty} {n};" for n, ty, w in sfields)
    decls = "\n".join(f"    {ty} {n};" for n, ty, w in bvars)

    def widen(ref, ty, w):
        return w.format(v=ref)

    # internal helper: mutate a packed numeric field, return its widened value
    helpers, hnames = [], []
    for i, (ref, ty, w) in enumerate(all_num):
        op = rng.choice(["+=", "-=", "*=", "^=", "|="])
        helpers.append(f"""
    function h{i}({ty} v) internal returns (uint256) {{
        unchecked {{ {ref} {op} v; }}
        return {widen(ref, ty, w)};
    }}""")
        hnames.append((f"h{i}", ty, ref, w))

    fns = []
    for i in range(rng.randrange(4, 7)):
        # compose a mutating helper with a neighbour read, mixed order
        hn, hty, href, hw = rng.choice(hnames) if hnames else ("", "uint8", "", "")
        rref, rty, rw = rng.choice(all_read)
        rd = widen(rref, rty, rw)
        # cast uint256 x to the helper's param type: uintN narrows directly,
        # intN must go via same-width uintN (solc rejects uint256->intN).
        if hty.startswith("int"):
            cast = f"{hty}(uint{hty[3:]}(x))"
        else:
            cast = f"{hty}(x)"
        callh = f"{hn}({cast})"
        forms = [
            f"{callh} + {rd}",                       # right-first: read rref BEFORE mutation
            f"{rd} + {callh}",                       # left-first read, then mutate
            f"({callh} > {rd}) ? {rd} : {callh}",    # ternary cond mutates, branches read
            f"{callh} * 3 + {rd}",
        ]
        body = rng.choice(forms)
        fns.append(f"""
    function f{i}(uint256 x) external returns (uint256) {{
        unchecked {{ return {body}; }}
    }}""")

    # a direct packed inc/dec + compound to exercise the write path standalone
    for i, (ref, ty, w) in enumerate(all_num[:3]):
        fns.append(f"""
    function inc{i}() external {{ unchecked {{ {'++' + ref if rng.random()<0.5 else ref + ' += 1'}; }} }}""")

    getters = "".join(f"""
    function get_{n.replace('.', '_')}() external view returns ({ty}) {{ return {n}; }}"""
        for n, ty, w in [("st." + a, b, c) for a, b, c in sfields] + bvars)

    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_combo.py seed={seed} — packed×sequencing×side-effect fixture (generated).
contract ComboFuzz {{
    struct S {{{smembers} }}
    S st;
{decls}
{''.join(helpers)}
{''.join(fns)}
{getters}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 80)
    seed0 = opt("--seed", 30000)
    max_per_fn = opt("--max-per-fn", 6)

    outdir = HERE / "out_combo"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"combo_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="ComboFuzz", max_per_fn=max_per_fn,
                                  harness=harness, quiet=True)
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
    print(f"COMBO CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
