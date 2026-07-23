#!/usr/bin/env python3
"""SEQUENCING-seam differential campaign — side-effecting calls INSIDE expressions.

Targets the OperandPlan work (v459+): legacy solc evaluates binop RIGHT operand
first, assignments RHS-first (store wins over callee write-backs), call args
left-to-right with write-backs visible to later args, ternary/&& conditions
first. Generates contracts whose mutator bodies compose two callee styles —
  - bumpDirect(v): writes state DIRECTLY (handle model, invisible to the
    pending-delta capture — covered by the EffectScan static scan), and
  - mutParam(s, v): `S storage` param (write-back / runtime-handle machinery)
— into binops, indexed/compound assignments, two-arg calls, ternaries and
short-circuits, interleaved with state reads whose value depends on WHEN the
sibling effect lands. State divergence is observed by the stateful runner's
zero-arg `obs()` getter after every mutation; revert status, event logs and
revert payloads are diffed by run_stateful_diff.

Known-issue skip: local post-dec on a dying value (`return a-- + a`) loses its
underflow panic to backend DCE (puyabug.md #8) — the generator never emits
`--`/`++` on locals whose value can underflow, and keeps operands bounded so
checked-arithmetic reverts stay rare (revert-status is still diffed when hit).

Usage: python fuzz_seq.py [--contracts N] [--funcs K] [--seed S] [--max-per-fn N]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import random
import sys
from pathlib import Path

from fuzz_state import run_stateful_diff, Harness, LocalNet, HERE

# no raw '-': two literal operands const-fold negative -> solc TypeError;
# order-sensitive subtraction goes through subCap() instead
OPS = ["+", "+", "*", "|", "&", "^"]
CMPS = [">", "<", ">=", "<=", "==", "!="]
ASSIGN_OPS = ["+=", "+=", "^=", "|=", "="]


class Gen:
    def __init__(self, rng):
        self.rng = rng

    def atom(self):
        r = self.rng.random()
        if r < 0.20:
            return self.rng.choice(["x", "y"])
        if r < 0.35:
            return str(self.rng.randrange(1, 9))
        if r < 0.55:
            return self.rng.choice(["s.f", "s.g", "cnt", "sacc % 251"])
        if r < 0.70:
            return f"arr[{self.rng.randrange(4)}]"
        if r < 0.85:
            return f"m[{self.rng.randrange(1, 4)}]"
        return self.rng.choice(["a", "b"])

    def call(self):
        r = self.rng.random()
        if r < 0.40:
            return f"bumpDirect({self.atom()} % 97)"
        if r < 0.75:
            return f"mutParam(s, {self.atom()} % 89)"
        return "inc()"

    def expr(self, depth, force_call=False):
        if depth <= 0:
            return self.call() if force_call else self.atom()
        r = self.rng.random()
        if force_call or r < 0.30:
            # a call somewhere in this subtree, composed with a state read —
            # the shape whose value depends on evaluation ORDER
            left = self.expr(depth - 1, force_call=self.rng.random() < 0.6)
            right = self.expr(depth - 1)
            if self.rng.random() < 0.5:
                left, right = right, left
            return f"({left} {self.rng.choice(OPS)} {right})"
        if r < 0.45:
            c = f"({self.expr(depth - 1)} % 2 == 1)"
            return f"({c} ? {self.expr(depth - 1)} : {self.expr(depth - 1)})"
        if r < 0.55:
            return (f"((({self.call()} % 2 == 0) && ({self.expr(depth - 1)} % 3 != 1)) "
                    f"? {self.atom()} : {self.expr(depth - 1)})")
        if r < 0.65:
            # order-sensitive guarded subtraction
            return (f"subCap({self.expr(depth - 1, force_call=force_call)}, "
                    f"{self.expr(depth - 1)})")
        return f"({self.expr(depth - 1)} {self.rng.choice(OPS)} {self.expr(depth - 1)})"

    def stmt(self):
        r = self.rng.random()
        if r < 0.20:
            return f"sacc = (sacc + ({self.expr(2, force_call=True)})) % 1000000007;"
        if r < 0.40:
            idx = f"({self.expr(1)}) % 4" if self.rng.random() < 0.5 else str(self.rng.randrange(4))
            return f"arr[{idx}] {self.rng.choice(ASSIGN_OPS)} ({self.expr(2, force_call=True)}) % 100003;"
        if r < 0.55:
            tgt = self.rng.choice(["s.f", "s.g", f"m[{self.rng.randrange(1, 4)}]"])
            return f"{tgt} {self.rng.choice(ASSIGN_OPS)} ({self.expr(2, force_call=True)}) % 100003;"
        if r < 0.70:
            return (f"sacc = (sacc + two({self.expr(1, force_call=True)}, "
                    f"{self.expr(1)})) % 1000000007;")
        if r < 0.85:
            return (f"if (({self.expr(1, force_call=True)}) % 2 == 1) "
                    f"{{ sacc = (sacc + {self.atom()}) % 1000000007; }}")
        return (f"a = ({self.expr(2, force_call=True)}) % 65521; "
                f"b = (b + a) % 65521;")


def gen_contract(seed, n_funcs):
    rng = random.Random(seed)
    g = Gen(rng)
    fns = []
    for i in range(n_funcs):
        body = "\n        ".join(g.stmt() for _ in range(rng.randrange(2, 5)))
        fns.append(f"""
    function f{i}(uint16 x, uint16 y) external {{
        uint256 a = uint256(x) + 3;
        uint256 b = uint256(y) + 5;
        {body}
        sacc = (sacc + a + b) % 1000000007;
    }}""")
    return f"""// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// fuzz_seq.py seed={seed} — sequencing-seam differential fixture (generated).
contract SeqFuzz {{
    struct S {{ uint256 f; uint256 g; }}
    S s;
    uint256 cnt;
    uint256 sacc;
    uint256[] arr;
    mapping(uint256 => uint256) m;

    constructor() {{
        arr.push(1); arr.push(2); arr.push(3); arr.push(4);
        s.f = 10; s.g = 20; m[1] = 7; m[2] = 8; m[3] = 9;
    }}

    function bumpDirect(uint256 v) internal returns (uint256) {{
        s.f = (s.f + v + 1) % 1000003;
        cnt += 1;
        return s.f % 251;
    }}

    function mutParam(S storage p, uint256 v) internal returns (uint256) {{
        p.g = (p.g + v + 2) % 1000033;
        return p.g % 241;
    }}

    function inc() internal returns (uint256) {{
        cnt += 1;
        return cnt % 199;
    }}

    function two(uint256 p, uint256 q) internal pure returns (uint256) {{
        return (p * 131 + q) % 1000000007;
    }}

    function subCap(uint256 p, uint256 q) internal pure returns (uint256) {{
        return p >= q ? p - q : q - p;
    }}
{''.join(fns)}

    function obs() external view returns (uint256, uint256, uint256, uint256, uint256, uint256) {{
        return (cnt, sacc, s.f, s.g,
            arr[0] + arr[1] * 100003 + arr[2] * 15485863 + arr[3],
            m[1] + m[2] * 100003 + m[3] * 15485863);
    }}
}}
"""


def main():
    argv = list(sys.argv[1:])
    def opt(name, default, cast=int):
        if name in argv:
            i = argv.index(name); v = cast(argv[i + 1]); del argv[i:i + 2]; return v
        return default
    n_contracts = opt("--contracts", 40)
    n_funcs = opt("--funcs", 5)
    seed0 = opt("--seed", 90000)
    max_per_fn = opt("--max-per-fn", 6)

    outdir = HERE / "out_seq"
    outdir.mkdir(exist_ok=True)
    ln = LocalNet()
    harness = Harness(ln, outdir)

    findings, errors = [], []
    for i in range(n_contracts):
        seed = seed0 + i
        fixture = outdir / f"seq_{seed}.sol"
        fixture.write_text(gen_contract(seed, n_funcs))
        print(f"\n[{i + 1}/{n_contracts}] seed={seed}")
        try:
            r = run_stateful_diff(fixture, entry="SeqFuzz", max_per_fn=max_per_fn,
                                  budget_pool=6, harness=harness, quiet=True)
        except KeyboardInterrupt:
            raise
        except BaseException as e:  # incl. SystemExit from the runner/oracle
            errors.append((seed, type(e).__name__ + ": " + str(e)[:200]))
            print(f"  ⚠️ runner error: {errors[-1][1]}")
            continue
        n_div = len(r["diverged"]) + len(r["event_div"]) + len(r["revert_div"])
        if n_div or r["avm_errors"]:
            findings.append((seed, r))
            print(f"  ❌ seed {seed}: {len(r['diverged'])} value / "
                  f"{len(r['event_div'])} event / {len(r['revert_div'])} revert-payload "
                  f"divergences, {len(r['avm_errors'])} AVM errors")
            for sig, args, exp, act in r["diverged"][:6]:
                print(f"     {sig}{tuple(args)}  evm={exp}  avm={act}")
            for sig, args, err in r["avm_errors"][:4]:
                print(f"     AVM-ERR {sig}{tuple(args)}: {err}")
        else:
            print(f"  ✅ {r['diffed']} calls clean")

    print("\n" + "=" * 60)
    print(f"CAMPAIGN DONE: {n_contracts} contracts, "
          f"{len(findings)} with findings, {len(errors)} runner errors")
    for seed, r in findings:
        print(f"  seed {seed}: {len(r['diverged'])}d/{len(r['event_div'])}e/"
              f"{len(r['revert_div'])}r/{len(r['avm_errors'])}err")
    for seed, e in errors:
        print(f"  runner-error seed {seed}: {e}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
