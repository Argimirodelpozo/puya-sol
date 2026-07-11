#!/usr/bin/env python3
"""NESTED-AGGREGATE differential campaign — random nested structs x mappings x funcptrs.

The hand probes (nested_combo_probe{,2}.sol) cover each combo once; this generates RANDOM
shapes for breadth: struct trees of depth 2-3 with mixed leaf widths (signed narrow, bool,
address, bytesN, uint128/256), dyn-array fields at random depths, containers (bare storage
var / mapping / nested mapping / struct array), deep leaf reads+writes+compound ops, whole
and partial aggregate copies, deletes at random depth, memory round-trips, and internal
function pointers (bare, in mappings, in fixed arrays, struct-taking) dispatched over the
generated struct types.

  python fuzz_nested.py [--fixtures N] [--seed S] [--max-per-fn M]

Sequential; unique contract names per fixture. Exit 1 if any fixture diverges/errors.
"""
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

LEAF_TYPES = ["int8", "int32", "int64", "int128", "uint16", "uint64", "uint128", "uint256",
              "bool", "address", "bytes4", "bytes32"]
NUM_LEAVES = [t for t in LEAF_TYPES if t not in ("bool", "address", "bytes4", "bytes32")]


def zero(ty):
    if ty == "bool":
        return "false"
    if ty == "address":
        return "address(0)"
    if ty.startswith("bytes") and ty != "bytes":
        return f"{ty}(0)"
    return f"{ty}(0)"


def lit(ty, rng):
    if ty == "bool":
        return rng.choice(["true", "false"])
    if ty == "address":
        return f"address(uint160(0x{rng.randrange(1, 2**32):x}))"
    if ty.startswith("bytes"):
        n = int(ty[5:])
        return f"{ty}(bytes{n}(uint{n*8}(0x{rng.randrange(1, 2**(min(n,4)*8)):x})))" if n <= 32 else "0"
    if ty.startswith("int"):
        bits = int(ty[3:] or 256)
        m = min(bits - 1, 32)
        return str(rng.randrange(-(2**m), 2**m))
    bits = int(ty[4:] or 256)
    m = min(bits, 32)
    return str(rng.randrange(0, 2**m))


def gen_fixture(tag, rng):
    # Struct tree: L0 (leaves) inside L1 (maybe + dyn array field) inside L2.
    l0_fields = [(f"a{i}", rng.choice(LEAF_TYPES)) for i in range(rng.randint(2, 4))]
    arr_leaf = rng.choice(NUM_LEAVES)
    l1_has_arr = rng.random() < 0.7
    l2_extra = rng.choice(NUM_LEAVES)

    l0_def = "struct L0 { " + " ".join(f"{t} {n};" for n, t in l0_fields) + " }"
    l1_def = ("struct L1 { L0 inner; " + (f"{arr_leaf}[] xs; " if l1_has_arr else "")
              + "bool flag; }")
    l2_def = f"struct L2 {{ {l2_extra} tag; L1 mid; }}"

    container = rng.choice(["bare", "map", "mapmap", "array"])
    decls = {"bare": "L2 s;", "map": "mapping(uint256 => L2) m;",
             "mapmap": "mapping(uint256 => mapping(uint8 => L2)) mm;",
             "array": "L2[] arr;"}[container]
    root = {"bare": "s", "map": "m[k]", "mapmap": "mm[k][uint8(k >> 8)]",
            "array": "arr[k % arr.length]"}[container]
    karg = "" if container == "bare" else "uint256 k, "
    kpass = "" if container == "bare" else "k, "

    fns = []
    if container == "array":
        fns.append("function grow() external { arr.push(); }")

    guard = "" if container != "array" else "if (arr.length == 0) return; "
    # per-leaf setters/getters + compound on numeric leaves
    for name, ty in l0_fields:
        fns.append(f"function set_{name}({karg}{ty} v) external {{ {guard}{root}.mid.inner.{name} = v; }}")
        ret_guard = "" if container != "array" else f"if (arr.length == 0) return {zero(ty)}; "
        fns.append(f"function get_{name}({karg.rstrip(', ') or 'uint256 _k'}) external view returns ({ty}) {{ {ret_guard}return {root if container != 'bare' else 's'}.mid.inner.{name}; }}"
                   .replace("m[k]", "m[k]") if container != "bare" else
                   f"function get_{name}(uint256 _k) external view returns ({ty}) {{ return s.mid.inner.{name}; }}")
        if ty.lstrip("u").startswith("int"):
            fns.append(f"function bump_{name}({karg}{ty} d) external {{ {guard}unchecked {{ {root}.mid.inner.{name} += d; }} }}")
    fns.append(f"function set_tag({karg}{l2_extra} v) external {{ {guard}{root}.tag = v; }}")
    tag_guard = "" if container != "array" else f"if (arr.length == 0) return {zero(l2_extra)}; "
    fns.append(f"function get_tag({karg.rstrip(', ') or 'uint256 _k'}) external view returns ({l2_extra}) {{ {tag_guard}return {root if container != 'bare' else 's'}.tag; }}")
    if l1_has_arr:
        fns.append(f"function push_xs({karg}{arr_leaf} v) external {{ {guard}{root}.mid.xs.push(v); }}")
        xs_guard = f"if ({root if container != 'bare' else 's'}.mid.xs.length <= i) return {zero(arr_leaf)}; "
        pre = guard if container == "array" else ""
        fns.append(f"function get_xs({karg}uint256 i) external view returns ({arr_leaf}) {{ {pre.replace('return;', f'return {zero(arr_leaf)};')}{xs_guard}return {root if container != 'bare' else 's'}.mid.xs[i]; }}")
    # whole/partial copies + delete + memory round-trip
    fns.append(f"function del_mid({karg.rstrip(', ') or ''}) external {{ {guard}delete {root}.mid; }}")
    fns.append(f"function roundtrip({karg}{l2_extra} nt) external {{ {guard}L2 memory t = {root}; t.tag = nt; {root} = t; }}")

    # struct-taking function pointers dispatched over the generated struct: pick the
    # first sub-64-bit numeric leaf as the observable (widening casts keep it exact)
    numeric0 = next(((n, t) for n, t in l0_fields
                     if t in ("int8", "int32", "int64", "uint16", "uint64")), None)
    if numeric0:
        n0, t0 = numeric0
        widen = f"int64(uint64(p.{n0}))" if t0.startswith("u") else f"int64(p.{n0})"
        fns.append(f"function pickA(L0 memory p) internal pure returns (int64) "
                   f"{{ unchecked {{ return {widen} * 2; }} }}")
        fns.append(f"function pickB(L0 memory p) internal pure returns (int64) "
                   f"{{ unchecked {{ return {widen} - 7; }} }}")
        fns.append("function(L0 memory) internal pure returns (int64) chosen;")
        fns.append("function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }")
        sel_guard = "" if container != "array" else "if (arr.length == 0) return -1; "
        fns.append(f"function callChosen({karg.rstrip(', ') or 'uint256 _k'}) external returns (int64) {{ {sel_guard}return chosen({root if container != 'bare' else 's'}.mid.inner); }}")

    nl = "\n    "
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag {tag}, container {container}).
contract G_{tag} {{
    {l0_def}
    {l1_def}
    {l2_def}
    {decls}
    {nl.join(fns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    n, seed, mpf = 8, 1, 10
    if "--fixtures" in argv:
        i = argv.index("--fixtures"); n = int(argv[i + 1]); del argv[i:i + 2]
    if "--seed" in argv:
        i = argv.index("--seed"); seed = int(argv[i + 1]); del argv[i:i + 2]
    if "--max-per-fn" in argv:
        i = argv.index("--max-per-fn"); mpf = int(argv[i + 1]); del argv[i:i + 2]

    bad = []
    for i in range(n):
        tag = f"nst{seed}_{i}"
        rng = random.Random(seed * 7919 + i)
        path = HERE / "contracts" / f"_{tag}.sol"
        path.write_text(gen_fixture(tag, rng))
        r = subprocess.run([sys.executable, str(HERE / "fuzz_state.py"), str(path),
                            "--contract", f"G_{tag}", "--max-per-fn", str(mpf)],
                           capture_output=True, text=True, timeout=1200)
        tail = (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "?"
        print(f"[{tag}] {'ok' if r.returncode == 0 else 'FINDINGS'}: {tail[:150]}", flush=True)
        if r.returncode != 0:
            bad.append(tag)
    print(f"=== {n - len(bad)}/{n} clean ===")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
