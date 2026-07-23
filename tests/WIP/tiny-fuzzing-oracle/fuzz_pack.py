#!/usr/bin/env python3
"""STORAGE-PACKING differential campaign — packed slot read-modify-write.

EVM packs multiple sub-32-byte state vars into one slot; a write to one field
is a read-modify-write that must NOT clobber its slot neighbours, and a read of
a signed sub-word field must sign-extend. This driver generates contracts with
random packed scalar fields (sub-word signed/unsigned ints, bool, bytesN) —
both bare state vars AND a packed struct — with plain setters, compound
+=/-=/*= (decode-before-arith), and zero-arg getters. run_stateful_diff
interleaves the mutators and re-reads every getter after each, diffing the
persisted values (+ revert status on checked overflow) against live solc+py-evm.

Usage: python fuzz_pack.py [--contracts N] [--seed S] [--max-per-fn N]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# (soltype, kind)  kind ∈ num / bool / bytesN
SCALARS = [
    ("uint8", "num"), ("int8", "num"), ("uint16", "num"), ("int16", "num"),
    ("uint24", "num"), ("int24", "num"), ("uint32", "num"), ("int40", "num"),
    ("uint64", "num"), ("int64", "num"), ("uint128", "num"), ("int128", "num"),
    ("bool", "bool"), ("bytes1", "bytesN"), ("bytes4", "bytesN"),
    ("bytes8", "bytesN"), ("bytes16", "bytesN"),
]
COMPOUND = ["+=", "-=", "*=", "&=", "|=", "^="]


def gen_contract(seed):
    rng = random.Random(seed)
    n = rng.randrange(3, 7)
    fields = [(f"f{i}", *rng.choice(SCALARS)) for i in range(n)]

    decls, fns = [], []
    for nm, ty, kind in fields:
        decls.append(f"    {ty} {nm};")
        fns.append(f"""
    function set_{nm}({ty} v) external {{ {nm} = v; }}
    function get_{nm}() external view returns ({ty}) {{ return {nm}; }}""")
        if kind == "num":
            op = rng.choice(COMPOUND)
            body = f"{nm} {op} v;"
            # unchecked variant sometimes to also exercise wrap semantics
            if rng.random() < 0.5:
                body = f"unchecked {{ {nm} {op} v; }}"
            fns.append(f"""
    function op_{nm}({ty} v) external {{ {body} }}""")

    # A packed struct + per-field set/get (struct-field COW rebuild path).
    sfields = [(f"s{i}", *rng.choice(SCALARS)) for i in range(rng.randrange(2, 5))]
    smembers = "".join(f" {ty} {nm};" for nm, ty, kind in sfields)
    sfns = []
    for nm, ty, kind in sfields:
        sfns.append(f"""
    function set_st_{nm}({ty} v) external {{ st.{nm} = v; }}
    function get_st_{nm}() external view returns ({ty}) {{ return st.{nm}; }}""")
        if kind == "num" and rng.random() < 0.5:
            sfns.append(f"""
    function op_st_{nm}({ty} v) external {{ unchecked {{ st.{nm} += v; }} }}""")

    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_pack.py seed={seed} — storage-packing differential fixture (generated).
contract PackFuzz {{
{chr(10).join(decls)}
    struct S {{{smembers} }}
    S st;
{''.join(fns)}
{''.join(sfns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 80)
    seed0 = opt("--seed", 60000)
    max_per_fn = opt("--max-per-fn", 6)

    outdir = HERE / "out_pack"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"pack_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="PackFuzz", max_per_fn=max_per_fn,
                                  harness=harness, quiet=True)
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            msg = str(e)[:200]
            if "solc" in msg.lower() or "compil" in msg.lower():
                skips += 1
                print(f"  ~ compile/oracle skip: {msg[:110]}")
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
    print(f"PACK CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/"
              f"{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
