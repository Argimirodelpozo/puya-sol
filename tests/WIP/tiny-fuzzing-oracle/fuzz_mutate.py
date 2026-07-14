#!/usr/bin/env python3
"""CORPUS-MUTATION differential fuzzer.

The generative fuzzers (fuzz_gen/fuzz_ternary) build random programs from a
grammar — they only explore shapes the grammar author imagined. This driver
takes the ~1450 SEMANTIC-TEST fixtures (real programs we already compile+run
green on BOTH solc+EVM and puya-sol+AVM) as a seed corpus and MUTATES them at
the Solidity-source level, keeping the differential oracle as the judge:

  seed fixture ──strip test directives──> baseline
       │  (must run CLEAN through the oracle, else it's not a trustworthy seed)
       ▼
  mutate (type width/sign flip) ──> validity gate (solc must still accept)
       ▼
  run_stateful_diff(mutant)  ──> EVM recomputes expected; AVM must match
       ▼
  ANY divergence on a valid mutant of a clean seed = a real miscompile.

The EVM side recomputes ground truth from the MUTATED program, so we never need
the fixture's `// ----` expectations — only that both back ends agree.

Mutation operator #1 (MVP): GLOBAL integer-type rewrite. Pick an intN/uintN
type present in the source, rewrite EVERY occurrence to a random other integer
type (width and/or sign). Consistent → high validity; stresses the full
width×sign sub-word codec + sign-extension matrix on REAL control flow, which
is exactly where puya-sol bugs have historically lived.

Usage:
  python fuzz_mutate.py [--dirs arithmetics,integer,...] [--seed S]
                        [--fixtures N] [--mutants M] [--max-per-fn K]
Single-threaded only (shared .compile_cache — concurrent runs poison it).
"""
import random
import re
import sys
import traceback
from pathlib import Path

from fuzz_evm import HERE, Harness, LocalNet
from fuzz_state import run_stateful_diff

CORPUS = HERE.parent.parent / "solidity-semantic-tests" / "tests"

# Categories rich in intN/uintN computation with fuzzable public functions.
DEFAULT_DIRS = [
    "arithmetics", "integer", "conversions", "cleanup", "exponentiation",
    "expressions", "constants", "shifts", "operators", "signed",
]

INT_TYPES = ([f"uint{b}" for b in range(8, 257, 8)]
             + [f"int{b}" for b in range(8, 257, 8)])
# `uint`/`int` bare aliases handled separately (they mean 256).

_TYPE_TOK = re.compile(r"\b(u?int)(\d*)\b")
_DIRECTIVE = re.compile(r"^\s*//\s*(====|----|.*:).*$", re.MULTILINE)


def strip_directives(src: str) -> str:
    """Remove the isoltest tail (`// ----` expectations) and `// ====` config so
    only the contract source remains. We cut everything from the first `// ----`
    or `// ====` line onward (the fixture body always precedes them)."""
    cut = len(src)
    for marker in ("\n// ----", "\n// ===="):
        i = src.find(marker)
        if i != -1:
            cut = min(cut, i)
    return src[:cut].rstrip() + "\n"


def present_int_types(src: str) -> set[str]:
    """Concrete intN/uintN types appearing in the source (bare uint/int → 256)."""
    out = set()
    for m in _TYPE_TOK.finditer(src):
        base, bits = m.group(1), m.group(2)
        out.add(f"{base}{bits}" if bits else f"{base}256")
    return out


def rewrite_type(src: str, frm: str, to: str) -> str:
    """Globally rewrite every occurrence of integer type `frm` to `to`, including
    the bare `uint`/`int` alias when frm is the 256-bit form."""
    fb = re.match(r"(u?int)(\d+)", frm)
    base, bits = fb.group(1), fb.group(2)
    # exact-width token, e.g. \buint128\b
    src = re.sub(rf"\b{base}{bits}\b", to, src)
    if bits == "256":
        # bare alias `uint`/`int` (not followed by a digit) also means this type
        src = re.sub(rf"\b{base}(?!\d)(?!{re.escape(to[len(base):])})", to, src)
    return src


def is_single_source(src: str) -> bool:
    return "==== ExternalSource" not in src and "==== Source" not in src


def eligible_fixtures(dirs):
    """Corpus fixtures that are single-source and contain at least one concrete
    integer type to mutate."""
    out = []
    for d in dirs:
        cdir = CORPUS / d / "contracts"
        if not cdir.is_dir():
            continue
        for f in sorted(cdir.glob("*.sol")):
            raw = f.read_text(errors="replace")
            if not is_single_source(raw):
                continue
            body = strip_directives(raw)
            if present_int_types(body):
                out.append(f)
    return out


def gen_mutants(body: str, rng, k: int):
    """Yield up to k (description, mutated_src) global type-rewrite mutants."""
    present = sorted(present_int_types(body))
    if not present:
        return
    seen = set()
    tries = 0
    while len(seen) < k and tries < k * 6:
        tries += 1
        frm = rng.choice(present)
        to = rng.choice([t for t in INT_TYPES if t != frm])
        key = (frm, to)
        if key in seen:
            continue
        seen.add(key)
        yield f"{frm}->{to}", rewrite_type(body, frm, to)


def _run(tmp: Path, src: str, harness, max_per_fn):
    """Write src to tmp and run the differential; return result dict or an error
    marker dict. solc-reject (validity fail) and puya-sol compile errors both
    surface here and are classified by the caller."""
    tmp.write_text(src)
    try:
        return run_stateful_diff(tmp, max_per_fn=max_per_fn, harness=harness, quiet=True)
    except SystemExit as e:                       # oracle sys.exit → solc reject / no fuzzable fns
        return {"error": "oracle", "msg": str(e)[:200]}
    except Exception as e:                         # puya-sol compile / deploy failure
        return {"error": type(e).__name__, "msg": str(e)[:200]}


def main():
    argv = sys.argv[1:]

    def opt(flag, dflt, cast=int):
        if flag in argv:
            return cast(argv[argv.index(flag) + 1])
        return dflt

    dirs = opt("--dirs", DEFAULT_DIRS, lambda s: s.split(","))
    seed0 = opt("--seed", 7000)
    n_fixtures = opt("--fixtures", 40)
    n_mutants = opt("--mutants", 6)
    max_per_fn = opt("--max-per-fn", 10)

    rng = random.Random(seed0)
    fixtures = eligible_fixtures(dirs)
    rng.shuffle(fixtures)
    fixtures = fixtures[:n_fixtures]
    print(f"[corpus-mutate] {len(fixtures)} eligible fixtures from {dirs}; "
          f"seed {seed0}, {n_mutants} mutants/fixture")

    ln = LocalNet(); harness = Harness(ln, HERE / "out_mutate")
    tmp = HERE / "contracts" / "_mut.sol"

    seeds_ok = seeds_skip = mutants_run = mutants_valid = 0
    invalid = 0
    findings = []
    for f in fixtures:
        body = strip_directives(f.read_text(errors="replace"))
        # Gate 1: the ORIGINAL must run CLEAN (both sides agree) — else divergences
        # on its mutants can't be blamed on the mutation.
        base = _run(tmp, body, harness, max_per_fn)
        if "error" in base or not base.get("ok"):
            seeds_skip += 1
            continue
        seeds_ok += 1
        print(f"[seed OK] {f.parent.parent.name}/{f.name} "
              f"({base['n_calls']} calls) — mutating")
        for desc, msrc in gen_mutants(body, rng, n_mutants):
            res = _run(tmp, msrc, harness, max_per_fn)
            mutants_run += 1
            if "error" in res:
                invalid += 1                       # solc-reject or puya compile-fail (either is fine)
                continue
            mutants_valid += 1
            if not res.get("ok"):
                if res["diverged"]:
                    findings.append((f, desc, res["diverged"]))
                    print(f"  ❌ DIVERGENCE {f.name} [{desc}]:")
                    for sig, args, exp, act in res["diverged"][:6]:
                        print(f"       {sig}{args}  evm={exp}  avm={act}")
                elif res["avm_errors"]:
                    print(f"  ⚠️  AVM-error-only {f.name} [{desc}]: "
                          f"{res['avm_errors'][0][2]}")

    print("\n" + "=" * 70)
    print(f"seeds: {seeds_ok} clean / {seeds_skip} skipped | "
          f"mutants: {mutants_run} run, {mutants_valid} valid, {invalid} invalid | "
          f"DIVERGENCES: {len(findings)}")
    for f, desc, div in findings:
        print(f"  ❌ {f.parent.parent.name}/{f.name} [{desc}] × {len(div)}")
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
