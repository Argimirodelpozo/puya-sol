#!/usr/bin/env python3
"""ASM-IN-EXPRESSION differential campaign — asm results composed in Solidity.

An asm-bodied helper returns a biguint (asm-biguint-return model); an inline
`assembly { r := ... }` block writes a Solidity local. When those results feed a
LARGER Solidity expression (arithmetic, comparison, ternary, mixed with params
and native ops) they must type + value-compose correctly — the seam fuzz_seq
(side-effect ordering) and fuzz_cd (calldata layout) didn't stress. This driver
generates asm-bodied helpers over a pool of well-defined Yul ops (add/mul/sub-
guarded/and/or/xor/shl/shr/byte/signextend/mulmod/addmod) plus inline-asm-block
reads of params/state, and composes them (unchecked, to isolate VALUE
correctness) into public functions diffed vs live solc+py-evm.

Usage: python fuzz_asmexpr.py [--contracts N] [--seed S] [--max-per-fn N]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE


def _asm_helpers(rng):
    """A pool of asm-bodied uint256 helpers (name, solidity-source)."""
    helpers = {
        "aAdd": "function aAdd(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=add(a,b)}}",
        "aMul": "function aMul(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=mul(a,b)}}",
        "aSub": "function aSub(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=sub(a,b)}}",
        "aAnd": "function aAnd(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=and(a,b)}}",
        "aOr":  "function aOr(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=or(a,b)}}",
        "aXor": "function aXor(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=xor(a,b)}}",
        "aShl": "function aShl(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=shl(and(b,0xff),a)}}",
        "aShr": "function aShr(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=shr(and(b,0xff),a)}}",
        "aByte":"function aByte(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=byte(and(b,0x1f),a)}}",
        "aSext":"function aSext(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=signextend(and(a,0x1f),b)}}",
        "aMulm":"function aMulm(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=mulmod(a,b,0x10001)}}",
        "aAddm":"function aAddm(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=addmod(a,b,0x10001)}}",
        "aLt":  "function aLt(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=lt(a,b)}}",
        "aSlt": "function aSlt(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=slt(a,b)}}",
        "aEq":  "function aEq(uint256 a,uint256 b) internal pure returns(uint256 r){assembly{r:=eq(a,b)}}",
    }
    return helpers


class Gen:
    def __init__(self, rng, names):
        self.rng = rng
        self.names = names

    def atom(self):
        r = self.rng.random()
        if r < 0.4:
            return self.rng.choice(["x", "y", "z"])
        if r < 0.6:
            return str(self.rng.randrange(0, 65))
        if r < 0.75:
            return self.rng.choice(["sx", "sy"])  # state reads
        return f"0x{self.rng.randrange(1, 1<<32):x}"

    def helper_call(self, depth):
        h = self.rng.choice(self.names)
        return f"{h}({self.expr(depth-1)}, {self.expr(depth-1)})"

    def expr(self, depth):
        if depth <= 0:
            return self.atom()
        r = self.rng.random()
        if r < 0.35:
            return self.helper_call(depth)
        if r < 0.55:
            op = self.rng.choice(["+", "*", "&", "|", "^"])  # no bare '-': literal-literal goes negative
            return f"({self.expr(depth-1)} {op} {self.expr(depth-1)})"
        if r < 0.70:
            # inline-asm block result used in the expression
            return f"_inl({self.expr(depth-1)}, {self.expr(depth-1)})"
        if r < 0.85:
            c = f"({self.expr(depth-1)} {self.rng.choice(['<','>','==','<=','>='])} {self.expr(depth-1)})"
            return f"({c} ? {self.expr(depth-1)} : {self.expr(depth-1)})"
        return self.atom()


def gen_contract(seed):
    rng = random.Random(seed)
    helpers = _asm_helpers(rng)
    chosen = rng.sample(list(helpers), rng.randrange(4, 9))
    g = Gen(rng, chosen)

    fns = []
    for i in range(rng.randrange(4, 7)):
        body = g.expr(3)
        fns.append(f"""
    function f{i}(uint256 x, uint256 y, uint256 z) external view returns (uint256) {{
        unchecked {{ return {body}; }}
    }}""")

    helper_src = "\n    ".join(helpers[h] for h in chosen)
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_asmexpr.py seed={seed} — asm-in-expression differential fixture (generated).
contract AsmExprFuzz {{
    uint256 sx = 0x0123456789abcdef;
    uint256 sy = 42;

    {helper_src}

    // inline-asm-block result consumed inline: r := mulmod-ish then composed.
    function _inl(uint256 a, uint256 b) internal pure returns (uint256 r) {{
        assembly {{ r := add(mul(a, 3), and(b, 0xffff)) }}
    }}
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
    seed0 = opt("--seed", 50000)
    max_per_fn = opt("--max-per-fn", 10)

    outdir = HERE / "out_asmexpr"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors, skips = [], [], 0
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"ae_{seed}.sol"
        fixture.write_text(gen_contract(seed))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="AsmExprFuzz", max_per_fn=max_per_fn,
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
    print(f"ASMEXPR CAMPAIGN DONE: {n_contracts} contracts, {len(findings)} with findings, "
          f"{len(errors)} runner errors, {skips} skips")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
