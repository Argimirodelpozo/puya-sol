#!/usr/bin/env python3
"""MODIFIER + INHERITANCE/DISPATCH differential campaign — generated contract hierarchies.

puya-sol does heavy source-level transformation for dispatch that the value/expr
fuzzers never exercise: modifier INLINING (nesting order of stacked modifiers,
multiple `_;` placeholders, gated placeholders, side-effecting modifier args), C3
linearization of a base chain, and `super.f()` resolution to `__super_N`
subroutines. A dedicated MANUAL modifier axis already found two real inliner bugs
(ModifierBodyInliner nesting-order + multiple-`_;` shared-ptr aliasing); this
GENERATES the surface: random `C0 <- C1 <- ...` hierarchies whose methods carry
random modifier stacks, are virtual/overridden across the chain, and call super.

Each modifier and function body appends a distinct tag to a shared `uint256 log`
(unchecked, so it wraps mod 2^256 instead of reverting — always comparable). The
log therefore ENCODES the exact execution path: a wrong modifier nesting order, a
dropped/duplicated `_;`, a mis-resolved override, or a missing super leg all
produce a different log value. Entry = the most-derived contract; fuzz_state.py
introspects it, fuzzes a call sequence, and diffs the persisted log + every method
return against a live solc+EVM.

  python fuzz_dispatch.py [--fixtures N] [--seed S] [--max-per-fn M]

Sequential (compile-cache discipline); unique contract names per fixture. Exit 1 if
any fixture diverges.
"""
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def gen_hierarchy(tag: str, rng: random.Random) -> tuple[str, str]:
    """Return (source, entry_contract_name)."""
    depth = rng.randint(2, 4)            # contracts in the chain C0..C{depth-1}
    names = [f"K{tag}_{i}" for i in range(depth)]

    # ── modifiers defined in the base (all derived inherit them) ──────────────
    # Each writes a distinct tag around `_;`; the mix exercises the inliner's
    # nesting order, post-placeholder, double-placeholder, state gate, and args.
    mod_defs = [
        # name, formal-params, how a call site supplies args (fn arg `a` in scope)
        ("mPre",   "",             "",       "unchecked { log = log*100 + 11; } _;"),
        ("mPost",  "",             "",       "_; unchecked { log = log*100 + 12; }"),
        ("mBoth",  "",             "",       "unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; }"),
        ("mGate",  "",             "",       "if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; }"),
        ("mArg",   "uint256 v",    "ARG",    "unchecked { log = log*100 + 70 + (v % 9); } _;"),
        # DOUBLE-placeholder modifier `{ ... _; _; }` — re-enabled 2026-07-08 after the modifier
        # lowering was consolidated onto the solc-aligned subroutine chain (the old textual inliner
        # mis-CSE'd a multiple-`_;` body containing a call; the chain handles it correctly).
        ("mTwice", "",             "",       "unchecked { log = log*100 + 15; } _; _;"),
    ]

    def mod_decl(name, params, _kind, body):
        return f"    modifier {name}({params}) {{ {body} }}"

    # ── choose a random modifier STACK for a function (0..3 modifiers) ─────────
    def pick_stack():
        k = rng.randint(0, 3)
        stack = []
        for _ in range(k):
            name, params, kind, _body = rng.choice(mod_defs)
            if kind == "ARG":
                # side-effecting arg: bump() mutates log too, so arg-eval order shows
                arg = rng.choice(["a % 5", "bump(a)"])
                stack.append(f"{name}({arg})")
            else:
                stack.append(f"{name}()")
        return " ".join(stack)

    # ── function slots: base defines virtual; some derived override + super ────
    n_slots = rng.randint(2, 4)
    # for each slot, pick which contracts (by index) define/override it; C0 always does
    slots = []
    for s in range(n_slots):
        definers = [0]
        for i in range(1, depth):
            if rng.random() < 0.55:
                definers.append(i)
        slots.append(definers)

    # build each contract's body
    bodies: dict[int, list[str]] = {i: [] for i in range(depth)}
    for s, definers in enumerate(slots):
        fname = f"f{s}"
        base_tag = 20 + s          # distinct per slot
        # VOID mutator slots have no early return, so a double `_;` (mTwice) actually runs
        # the body TWICE — the exact shape of the historical multiple-placeholder inliner
        # bug — and the body does CHECKED arithmetic (`sum += ...`) which a mis-inlined /
        # shared-ptr-aliased body miscompiles. RETURNING slots test the super chain.
        is_void = rng.random() < 0.5
        for pos, ci in enumerate(definers):
            is_first = (pos == 0)
            is_last = (pos == len(definers) - 1)
            spec = []
            if is_first and not is_last:
                spec.append("virtual")
            elif not is_first and not is_last:
                spec.append("override")
                spec.append("virtual")
            elif not is_first and is_last:
                spec.append("override")
            elif is_first and is_last:
                pass  # single definer, no virtual/override
            stack = pick_stack()
            spec_str = (" " + " ".join(spec)) if spec else ""
            stack_str = (" " + stack) if stack else ""
            call_super = (not is_first) and rng.random() < 0.7
            contrib = f"unchecked {{ log = log*100 + {base_tag} + (a % 7); }}"
            if is_void:
                # checked accumulate (runs Nx under a double placeholder) + trace + super
                checked = f"sum += (a % 13) + 1;"
                sup = f" super.{fname}(a);" if call_super else ""
                bodies[ci].append(
                    f"    function {fname}(uint256 a) public{spec_str}{stack_str} "
                    f"{{ {contrib} {checked}{sup} }}")
            else:
                tail = f"return super.{fname}(a);" if call_super else "return log;"
                bodies[ci].append(
                    f"    function {fname}(uint256 a) public{spec_str}{stack_str} returns (uint256) "
                    f"{{ {contrib} {tail} }}")

    # assemble contracts
    out = ["// SPDX-License-Identifier: MIT", "pragma solidity ^0.8.0;",
           "// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag %s)." % tag]
    for i in range(depth):
        header = f"contract {names[i]}" + (f" is {names[i-1]}" if i > 0 else "")
        lines = [header + " {"]
        if i == 0:
            lines.append("    uint256 public log;")
            lines.append("    uint256 public sum;   // checked accumulator (exercises multi-run bodies)")
            # side-effecting helper for modifier args (mutates log, returns v)
            lines.append("    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }")
            for md in mod_defs:
                lines.append(mod_decl(*md))
        lines.extend(bodies[i])
        lines.append("}")
        out.append("\n".join(lines))
    return "\n".join(out), names[-1]


def main() -> None:
    argv = list(sys.argv[1:])
    n_fixtures, seed, max_per_fn = 10, 1, 12
    if "--fixtures" in argv:
        i = argv.index("--fixtures"); n_fixtures = int(argv[i + 1]); del argv[i:i + 2]
    if "--seed" in argv:
        i = argv.index("--seed"); seed = int(argv[i + 1]); del argv[i:i + 2]
    if "--max-per-fn" in argv:
        i = argv.index("--max-per-fn"); max_per_fn = int(argv[i + 1]); del argv[i:i + 2]

    failures = []
    for i in range(n_fixtures):
        tag = f"d{seed}_{i}"
        rng = random.Random(seed * 100003 + i)
        src, entry = gen_hierarchy(tag, rng)
        path = HERE / "contracts" / f"_{tag}.sol"
        path.write_text(src)
        print(f"### {tag} (entry {entry})", flush=True)
        r = subprocess.run(
            [sys.executable, str(HERE / "fuzz_state.py"), str(path),
             "--contract", entry, "--max-per-fn", str(max_per_fn)],
            capture_output=True, text=True, timeout=1200)
        tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-6:])
        print(tail, flush=True)
        if r.returncode != 0:
            failures.append(tag)
            print(f"!!! DIVERGENCE OR ERROR in {tag} (see above)", flush=True)

    print(f"\n=== dispatch campaign done: {n_fixtures - len(failures)}/{n_fixtures} clean ===")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
