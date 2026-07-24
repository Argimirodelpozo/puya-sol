#!/usr/bin/env python3
"""DYNAMIC ABI-CODEC differential campaign — dynamic/nested aggregates ACROSS the
external ABI boundary (params AND returns).

Everything certified so far (fuzz_pack/cd/emit/revcamp) is scalar/static; fuzz_nested
is in-contract STORAGE manipulation. This is the uncovered surface: the ARC4↔EVM-ABI
codec for DYNAMIC values at the call boundary — dynamic arrays (`T[]`), `bytes`/`string`,
nesting (`T[][]`, `bytes[]`), multi-dynamic returns, and structs carrying dynamic fields.
Each generated function is a pure transform (echo / reverse / slice / concat / struct
round-trip), so the decoded RETURN value must round-trip through both encoders — a
divergence means a codec bug (offset headers, length prefixes, tail packing, signed
element width in dynamic layout). Diffed via run_stateful_diff (canon recurses lists/
bytes, so dynamic returns compare structurally).

Usage: python fuzz_abicodec.py [--contracts N] [--seed S] [--max-per-fn M]
"""
import random
import sys

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# element types for numeric dynamic arrays — mix of ARC4 element widths + signed sub-word
NUM_ELEMS = ["uint256", "uint128", "uint64", "int128", "int64", "int16", "uint8"]


def _echo_arr(i, rng):
    e = rng.choice(NUM_ELEMS)
    return f"""
    function f{i}({e}[] calldata a) external pure returns ({e}[] memory) {{ return a; }}"""


def _reverse_arr(i, rng):
    e = rng.choice(NUM_ELEMS)
    return f"""
    function f{i}({e}[] calldata a) external pure returns ({e}[] memory r) {{
        r = new {e}[](a.length);
        for (uint256 k = 0; k < a.length; k++) r[k] = a[a.length - 1 - k];
    }}"""


def _echo_bytes(i, rng):
    t = rng.choice(["bytes", "string"])
    return f"""
    function f{i}({t} calldata b) external pure returns ({t} memory) {{ return b; }}"""


def _head_bytes(i, rng):
    return f"""
    function f{i}(bytes calldata b, uint256 n) external pure returns (bytes memory) {{
        uint256 m = n > b.length ? b.length : n;
        return b[0:m];
    }}"""


def _concat_string(i, rng):
    return f"""
    function f{i}(string calldata a, string calldata b) external pure returns (string memory) {{
        return string.concat(a, b);
    }}"""


def _echo_nested(i, rng):
    e = rng.choice(["uint256", "int128", "uint64"])
    return f"""
    function f{i}({e}[][] calldata a) external pure returns ({e}[][] memory) {{ return a; }}"""


def _echo_bytesarr(i, rng):
    return f"""
    function f{i}(bytes[] calldata a) external pure returns (bytes[] memory) {{ return a; }}"""


def _multi_dyn(i, rng):
    e = rng.choice(NUM_ELEMS)
    return f"""
    function f{i}({e}[] calldata a, bytes calldata b) external pure
            returns ({e}[] memory, bytes memory) {{
        return (a, b);
    }}"""


def _echo_struct(i, rng):
    # struct carrying a dynamic array + bytes; round-trip through memory
    return f"""
    function f{i}(D calldata s) external pure returns (D memory) {{ return s; }}"""


TEMPLATES = [_echo_arr, _reverse_arr, _echo_bytes, _head_bytes, _concat_string,
             _echo_nested, _echo_bytesarr, _multi_dyn, _echo_struct]


def gen_contract(seed):
    rng = random.Random(seed)
    n = rng.randrange(4, 9)
    fns = []
    for i in range(n):
        fns.append(rng.choice(TEMPLATES)(i, rng))
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_abicodec.py seed={seed} — dynamic ABI-codec differential fixture (generated).
contract AbiFuzz {{
    struct D {{ uint64 x; int128[] arr; bytes data; }}
{''.join(fns)}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default):
        if name in argv:
            i = argv.index(name); v = int(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 80)
    seed0 = opt("--seed", 20000)
    max_per_fn = opt("--max-per-fn", 10)

    outdir = HERE / "out_abicodec"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"abi_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="AbiFuzz", max_per_fn=max_per_fn,
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
    print(f"ABICODEC CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
