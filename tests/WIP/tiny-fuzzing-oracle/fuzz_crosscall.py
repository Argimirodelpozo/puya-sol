#!/usr/bin/env python3
"""CROSS-CONTRACT differential campaign — generated Caller→Callee fixtures.

The hand-written cross-contract probes (external_call_signed_narrow_return.sol)
cover the shapes the signed-narrow-return fix family was built against. This
generates RANDOM cross-contract surfaces with the axes that were previously
RESTRICTED in the campaign generator while that family was open:

  - signed struct fields / array elements at ALL widths (int8..int256, not just int64)
  - uint128/uint256 tuple elements, mixed with signed narrow ones
  - 3-element tuples (harder offset arithmetic than pairs)
  - signed narrow ARGS across the call boundary
  - bool mixed into tuples/structs

Each fixture: `Cee_<tag>` (callee) + `Cer_<tag>` (caller; `new`s the callee in
its constructor and forwards typed `c.f(...)` calls, widening every result to a
single int256/uint256 observable). Entry is pinned with `--contract` on both
sides via fuzz_state.py, which fuzzes boundary inputs and diffs AVM vs a live
solc+EVM.

  python fuzz_crosscall.py [--fixtures N] [--seed S] [--max-per-fn M]

Sequential on purpose (compile-cache discipline); unique contract names per
fixture (cache-poisoning discipline). Exit 1 if any fixture diverges.
"""
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

SCALAR_INTS = ["int8", "int16", "int32", "int64", "int128", "int256"]
# bytes4/bytes32/address re-added 2026-07-06: the manual per-field tuple decoder was
# replaced with an ARC4Decode-based decode (SolExternalCall.cpp), which delegates
# head/tail/address-padding to puya and closes the address + dynamic-field cases the
# old walker mishandled. (enum still needs _cast_from_arg/_widen helper support before
# it can join here — the enum SELECTOR-name fix landed alongside, but the fixture
# helpers don't yet synthesise an enum value.)
ELEM_TYPES = ["int8", "int16", "int32", "int64", "int128", "int256",
              "uint8", "uint32", "uint64", "uint128", "uint256", "bool",
              "bytes4", "bytes32", "address"]
ARR_ELEMS = ["int8", "int16", "int32", "int64", "int128", "uint16", "uint128"]


def _bytesn_uint(ty: str) -> str:
    """`bytes4` -> `uint32` (the same-width unsigned int for a fixed-bytes type)."""
    return f"uint{int(ty[5:]) * 8}"


def _widen(expr: str, ty: str) -> str:
    """Widen a typed value to an int256 term for the observable sum."""
    if ty == "bool":
        return f"({expr} ? int256(1) : int256(0))"
    if ty.startswith("uint"):
        return f"int256(uint256({expr}))"
    if ty.startswith("bytes"):
        return f"int256(uint256({_bytesn_uint(ty)}({expr})))"
    if ty == "address":
        return f"int256(uint256(uint160({expr})))"
    return f"int256({expr})"


def _cast_from_arg(ty: str, arg: str) -> str:
    """Construct a value of `ty` from an int256/uint256 entry arg."""
    if ty == "bool":
        return f"({arg} % 2 == 0)"
    if ty.startswith("uint"):
        return f"{ty}(uint256({arg}))"
    if ty.startswith("bytes"):
        return f"{ty}({_bytesn_uint(ty)}(uint256({arg})))"
    if ty == "address":
        return f"address(uint160(uint256({arg})))"
    return f"{ty}({arg})"


def gen_fixture(tag: str, rng: random.Random) -> str:
    cee, cer, structs = [], [], []
    fn = 0

    def nm() -> str:
        nonlocal fn
        fn += 1
        return f"f{fn}"

    # 1-2 scalar signed returns (regression floor for the single-return fix)
    for _ in range(rng.randint(1, 2)):
        w = rng.choice(SCALAR_INTS)
        n = nm()
        cee.append(f"    function {n}(int256 a) external pure returns ({w}) {{ return {w}(a); }}")
        cer.append(f"    function g{n}(int256 a) external view returns (int256) {{ return int256(c.{n}(a)); }}")

    # 2 tuple returns, 2-3 elements, previously-restricted element mixes
    for _ in range(2):
        k = rng.randint(2, 3)
        tys = [rng.choice(ELEM_TYPES) for _ in range(k)]
        n = nm()
        args = ", ".join(f"int256 a{i}" for i in range(k))
        rets = ", ".join(tys)
        vals = ", ".join(_cast_from_arg(t, f"a{i}") for i, t in enumerate(tys))
        cee.append(f"    function {n}({args}) external pure returns ({rets}) {{ return ({vals}); }}")
        binds = ", ".join(f"{t} x{i}" for i, t in enumerate(tys))
        callargs = ", ".join(f"a{i}" for i in range(k))
        summ = " + ".join(_widen(f"x{i}", t) for i, t in enumerate(tys))
        cer.append(
            f"    function g{n}({args}) external view returns (int256) {{ "
            f"({binds}) = c.{n}({callargs}); return {summ}; }}")

    # 1 struct round-trip (return + re-pass as arg), mixed-width signed fields
    k = rng.randint(2, 4)
    tys = [rng.choice(ELEM_TYPES) for _ in range(k)]
    sname = f"S{tag}"
    fields = " ".join(f"{t} m{i};" for i, t in enumerate(tys))
    structs.append(f"struct {sname} {{ {fields} }}")
    n = nm()
    args = ", ".join(f"int256 a{i}" for i in range(k))
    vals = ", ".join(_cast_from_arg(t, f"a{i}") for i, t in enumerate(tys))
    summ = " + ".join(_widen(f"p.m{i}", t) for i, t in enumerate(tys))
    cee.append(f"    function {n}({args}) external pure returns ({sname} memory) {{ return {sname}({vals}); }}")
    cee.append(f"    function {n}x({sname} memory p) external pure returns (int256) {{ return {summ}; }}")
    cer.append(
        f"    function g{n}({args}) external view returns (int256) {{ "
        f"{sname} memory p = c.{n}({', '.join(f'a{i}' for i in range(k))}); return c.{n}x(p); }}")

    # 1 array round-trip (return + re-pass), previously-restricted element widths
    et = rng.choice(ARR_ELEMS)
    ln = rng.randint(2, 3)
    n = nm()
    mk = "".join(f"r[{i}] = {_cast_from_arg(et, f'a + int256({i})' if not et.startswith('uint') else f'a + int256({i})')}; " for i in range(ln))
    cee.append(
        f"    function {n}(int256 a) external pure returns ({et}[] memory) {{ "
        f"{et}[] memory r = new {et}[]({ln}); {mk}return r; }}")
    cee.append(
        f"    function {n}x({et}[] memory xs) external pure returns (int256) {{ "
        f"int256 s; for (uint i; i < xs.length; i++) s += {_widen('xs[i]', et)}; return s; }}")
    cer.append(f"    function g{n}(int256 a) external view returns (int256) {{ return c.{n}x(c.{n}(a)); }}")

    # 1 signed-narrow ARGS crossing the boundary
    aw, bw = rng.choice(SCALAR_INTS[:4]), rng.choice(SCALAR_INTS)
    n = nm()
    cee.append(
        f"    function {n}({aw} x, {bw} y) external pure returns (int256) {{ "
        f"return int256(x) + int256(y); }}")
    cer.append(
        f"    function g{n}(int256 a, int256 b) external view returns (int256) {{ "
        f"return c.{n}({aw}(a), {bw}(b)); }}")

    nl = "\n"
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag {tag}).
{nl.join(structs)}
contract Cee_{tag} {{
{nl.join(cee)}
}}
contract Cer_{tag} {{
    Cee_{tag} c;
    constructor() {{ c = new Cee_{tag}(); }}
{nl.join(cer)}
}}
"""


def main() -> None:
    argv = list(sys.argv[1:])
    n_fixtures, seed, max_per_fn = 8, 1, 20
    if "--fixtures" in argv:
        i = argv.index("--fixtures"); n_fixtures = int(argv[i + 1]); del argv[i:i + 2]
    if "--seed" in argv:
        i = argv.index("--seed"); seed = int(argv[i + 1]); del argv[i:i + 2]
    if "--max-per-fn" in argv:
        i = argv.index("--max-per-fn"); max_per_fn = int(argv[i + 1]); del argv[i:i + 2]

    failures = []
    for i in range(n_fixtures):
        tag = f"cc{seed}_{i}"
        rng = random.Random(seed * 1000 + i)
        src = gen_fixture(tag, rng)
        path = HERE / "contracts" / f"_{tag}.sol"
        path.write_text(src)
        print(f"### {tag}", flush=True)
        r = subprocess.run(
            [sys.executable, str(HERE / "fuzz_state.py"), str(path),
             "--contract", f"Cer_{tag}",
             "--max-per-fn", str(max_per_fn)],
            capture_output=True, text=True, timeout=1200)
        tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-6:])
        print(tail, flush=True)
        if r.returncode != 0:
            failures.append(tag)
            print(f"!!! DIVERGENCE OR ERROR in {tag} (see above)", flush=True)

    print(f"\n=== campaign done: {n_fixtures - len(failures)}/{n_fixtures} clean ===")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
