#!/usr/bin/env python3
"""Directed hunt for finding #15 ("3-deep ternary sub-word signed miscompile",
fuzz_gen seed 9104, unreproducible since the --depth generator change).

Reuses fuzz_gen's contract assembly + differential runner but monkey-patches
the expression generator into #15's territory:
  - TERNARY-DOMINANT trees (p=0.5, and the top TWO levels are forced ternaries
    so every function contains a >=3-deep ternary chain via nesting)
  - SUB-WORD SIGNED types only, including the odd widths (int24/int40) where
    the sign-extension codec has historically lived dangerously
  - binops weighted toward CHECKED SIGNED mul/div/mod (the
    ternary-operand-signed-mul class, cd9d91ccfa)
  - round-trip casts stay on (they interact with sign extension)

Usage: python fuzz_ternary.py [--contracts N] [--funcs K] [--seed S]
       (single-threaded — never run concurrently with other fuzz drivers)
"""
import sys

import fuzz_gen as G

TERN_TYPES = ["int8", "int16", "int24", "int32", "int40", "int64", "int128"]
HEAVY_BINOPS = ["*", "/", "%", "*", "/", "%", "+", "-", "&", "|", "^"]


def tern_expr(depth, rng, ty, vars=("a", "b", "c", "d"), force_ternary=0):
    signed = ty.startswith("int")
    if depth <= 0 or (force_ternary <= 0 and rng.random() < 0.30):
        return rng.choice(list(vars) + [f"type({ty}).{'min' if signed else 'max'}"])
    r = rng.random()
    if force_ternary > 0 or r < 0.50:                       # ternary-dominant
        cond = (f"({tern_expr(depth - 1, rng, ty, vars)} {rng.choice(G.CMPS)} "
                f"{tern_expr(depth - 1, rng, ty, vars)})")
        t = tern_expr(depth - 1, rng, ty, vars, force_ternary - 1)
        f = tern_expr(depth - 1, rng, ty, vars, force_ternary - 1)
        return f"({cond} ? {t} : {f})"
    if r < 0.60 and signed:                                 # unary minus (sub-word negate class)
        operand = tern_expr(depth - 1, rng, ty, vars)
        if operand == f"type({ty}).min":
            operand = rng.choice(list(vars))
        return f"(-{operand})"
    if G._CASTS and r < 0.70:                               # round-trip cast
        src = G._cast_src(ty, rng)
        return f"({ty}({src}({tern_expr(depth - 1, rng, ty, vars)})))"
    return (f"({tern_expr(depth - 1, rng, ty, vars)} {rng.choice(HEAVY_BINOPS)} "
            f"{tern_expr(depth - 1, rng, ty, vars)})")


def gen_expr_patched(depth, rng, ty, vars=("a", "b", "c", "d")):
    # top-level entry (gen_contract calls with depth=_DEPTH): force the first
    # two levels to be ternaries so every body nests ternary-in-ternary
    if depth >= G._DEPTH:
        return tern_expr(depth, rng, ty, vars, force_ternary=2)
    return tern_expr(depth, rng, ty, vars)


def main():
    G.TYPES[:] = TERN_TYPES
    G.gen_expr = gen_expr_patched
    G._CASTS = True
    # delegate arg parsing / running to fuzz_gen's main
    G.main()


if __name__ == "__main__":
    main()
