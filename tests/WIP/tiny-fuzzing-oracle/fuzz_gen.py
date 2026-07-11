#!/usr/bin/env python3
"""GENERATIVE differential fuzzer — fuzz the CODEGEN, not just inputs.

Hand-written probes test the shapes you think of; this generates RANDOM computational
Solidity (expression trees over every operator + shifts + exponent + ternary + comparisons),
compiles each on solc+EVM AND puya-sol+AVM, fuzzes the inputs, and diffs. A divergence (or a
puya-sol compile error on valid Solidity) prints the generating function source so the buggy
codegen pattern is reproducible by --seed.

  python fuzz_gen.py [--contracts N] [--funcs K] [--seed S] [--max-per-fn M]

Each contract packs K random functions (amortises the AVM deploy); operands stay in one type
domain (all uint256 or all int256) so the Solidity type-checks. Shift amounts + exponents are
literals (Solidity requires unsigned shift counts; keeps gas/opcode budget bounded).
"""
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from fuzz_evm import (_oracle, gen_calls, canon, _fmt, _fmt1, _apply_addr_canon,
                      REVERT, Harness, LocalNet)

BINOPS = ["+", "-", "*", "/", "%", "&", "|", "^"]
CMPS = ["<", "<=", ">", ">=", "==", "!="]


TYPES = ["uint8", "uint16", "uint64", "uint128", "uint256",
         "int8", "int16", "int64", "int128", "int256"]


BYTES_TYPES = ["bytes1", "bytes4", "bytes8", "bytes16", "bytes32"]


def gen_bytes_expr(depth, rng, ty, vars=("a", "b", "c", "d")):
    """bytesN expression tree — fixed-width bitwise (`& | ^ ~`) + bit shifts (by a literal uint).
    No arithmetic / indexing (keeps the result type bytesN). Exercises the AVM fixed-width byte
    codec (b~ must invert exactly N bytes, not pad to 32; shifts move bits within N bytes)."""
    if depth <= 0 or rng.random() < 0.4:
        return rng.choice(list(vars))
    r = rng.random()
    if r < 0.25:                                            # ~ (invert exactly N bytes)
        return f"(~{gen_bytes_expr(depth - 1, rng, ty, vars)})"
    if r < 0.50:                                            # shift by a literal bit count
        return f"({gen_bytes_expr(depth - 1, rng, ty, vars)} {rng.choice(['<<', '>>'])} {rng.choice(['0','1','8','16','255','256'])})"
    return f"({gen_bytes_expr(depth - 1, rng, ty, vars)} {rng.choice(['&', '|', '^'])} {gen_bytes_expr(depth - 1, rng, ty, vars)})"


_CASTS = False  # module toggle (--cast): insert round-trip casts ty->src->ty in gen_expr
_DEPTH = 4      # module toggle (--depth N): max expression-tree nesting depth (default 4)


def _cast_src(ty, rng):
    """A type reachable from `ty` by a SINGLE valid Solidity cast (same sign + different size, OR
    same size + opposite sign). Two-axis casts (e.g. int16(uint8)) are rejected by solc, so a
    round-trip ty->src->ty must keep each leg single-axis."""
    signed = ty.startswith("int")
    bits = ty[3:] if signed else ty[4:]
    pre = "int" if signed else "uint"
    same_sign = [pre + b for b in ["8", "16", "64", "128", "256"] if b != bits]
    opp_sign = [("uint" if signed else "int") + bits]
    return rng.choice(same_sign + opp_sign)


def gen_expr(depth, rng, ty, vars=("a", "b", "c", "d")):
    signed = ty.startswith("int")
    if depth <= 0 or rng.random() < 0.35:
        # VARIABLES only (+ the correctly-typed bound) — bare numeric literals constant-fold to a
        # wrong/narrow type (`~256`→negative for a uint return; small int → int8). The input fuzzer
        # already drives a/b/c/d across the type's 0/1/2/max/-1/min boundaries.
        return rng.choice(list(vars) + [f"type({ty}).{'min' if signed else 'max'}"])
    r = rng.random()
    if r < 0.12:                                            # unary (~ both; - signed only)
        op = rng.choice(["~", "-"]) if signed else "~"
        operand = gen_expr(depth - 1, rng, ty, vars)
        # Suppress the already-characterized const-neg divergence (`-type(intN).min` reverts on EVM,
        # overflow; documented in const-negate-typemin-divergence) — it floods the diff and masks new
        # findings. Substitute a variable for the bare type-min leaf under unary minus only.
        if op == "-" and operand == f"type({ty}).min":
            operand = rng.choice(list(vars))
        return f"({op}{operand})"
    if r < 0.22:                                            # ternary
        cond = f"({gen_expr(depth - 1, rng, ty, vars)} {rng.choice(CMPS)} {gen_expr(depth - 1, rng, ty, vars)})"
        return f"({cond} ? {gen_expr(depth - 1, rng, ty, vars)} : {gen_expr(depth - 1, rng, ty, vars)})"
    if r < 0.32:                                            # shift by a literal amount
        return f"({gen_expr(depth - 1, rng, ty, vars)} {rng.choice(['<<', '>>'])} {rng.choice(['0','1','7','8','255','256'])})"
    if r < 0.40:                                            # exponent by a small literal
        return f"({gen_expr(depth - 1, rng, ty, vars)} ** {rng.choice(['0','1','2','3'])})"
    if _CASTS and r < 0.52:                                 # round-trip cast ty->src->ty (narrow+rewiden / sign round-trip)
        src = _cast_src(ty, rng)
        return f"({ty}({src}({gen_expr(depth - 1, rng, ty, vars)})))"
    return f"({gen_expr(depth - 1, rng, ty, vars)} {rng.choice(BINOPS)} {gen_expr(depth - 1, rng, ty, vars)})"


# ── control-flow body generator ──
CF_VARS = ("a", "b", "c", "d", "acc")
ASSIGN_OPS = ["=", "+=", "-=", "*=", "|=", "&=", "^="]


def _cf_cond(rng, ty):
    return f"({gen_expr(2, rng, ty, CF_VARS)} {rng.choice(CMPS)} {gen_expr(2, rng, ty, CF_VARS)})"


def gen_stmt(depth, rng, ty, ctr):
    """A statement that mutates `acc`. ctr is a mutable [int] giving unique loop-var names
    (Solidity forbids shadowing a loop var in a nested loop)."""
    r = rng.random()
    if depth <= 0 or r < 0.45:                              # acc OP= expr
        return f"acc {rng.choice(ASSIGN_OPS)} {gen_expr(rng.randint(1, 3), rng, ty, CF_VARS)};"
    if r < 0.62:                                            # if / else
        return (f"if ({_cf_cond(rng, ty)}) {{ {gen_stmt(depth - 1, rng, ty, ctr)} }} "
                f"else {{ {gen_stmt(depth - 1, rng, ty, ctr)} }}")
    v = f"i{ctr[0]}"; ctr[0] += 1                           # bounded loop with optional break/continue
    n = rng.choice([2, 3, 4])
    cflow = ""
    c = rng.random()
    if c < 0.25:
        cflow = f"if ({_cf_cond(rng, ty)}) break; "
    elif c < 0.45:
        cflow = f"if ({_cf_cond(rng, ty)}) continue; "
    return f"for (uint {v} = 0; {v} < {n}; {v}++) {{ {cflow}{gen_stmt(depth - 1, rng, ty, ctr)} }}"


def gen_arr_stmt(rng, ty, ctr):
    """A statement using the array params `arr` (T[]) / `mat` (T[][]) inside loops — exercises
    `arr.length`/`mat[i].length` in loop conditions and `arr[i]`/`mat[i][j]` indexing (the
    loop-condition pre-statement class, fix 727f44ac2d)."""
    r = rng.random()
    if r < 0.45:                                           # 1D loop over arr (length cond + index)
        v = f"i{ctr[0]}"; ctr[0] += 1
        cflow = ""
        c = rng.random()
        if c < 0.2:
            cflow = f"if ({_cf_cond(rng, ty)}) break; "
        elif c < 0.35:
            cflow = f"if ({_cf_cond(rng, ty)}) continue; "
        return f"for (uint {v} = 0; {v} < arr.length; {v}++) {{ {cflow}acc {rng.choice(ASSIGN_OPS)} arr[{v}]; }}"
    if r < 0.8:                                            # NESTED loop over mat — the loop-cond class
        vi = f"i{ctr[0]}"; ctr[0] += 1
        vj = f"i{ctr[0]}"; ctr[0] += 1
        return (f"for (uint {vi} = 0; {vi} < mat.length; {vi}++) {{ "
                f"for (uint {vj} = 0; {vj} < mat[{vi}].length; {vj}++) {{ acc {rng.choice(ASSIGN_OPS)} mat[{vi}][{vj}]; }} }}")
    return gen_stmt(2, rng, ty, ctr)                       # plain scalar statement


STATE_NUM = ["uint256", "uint64", "int128", "uint8", "int16"]    # numeric → RMW-safe (op_)
STATE_OTHER = ["bytes32", "bool", "address"]                     # set/get only (no arithmetic)
STATE_SCALAR = STATE_NUM + STATE_OTHER
STATE_ARRAY = ["uint64", "uint256", "uint128", "int128", "uint8", "int16"]  # wide-element .length fixed 481f914810
                                                               # ([[wide-array-length-puya-bug]])


def _zero(ty):
    # typed zero so the array-getter ternary `cond ? arr[j] : _zero` type-matches the element type;
    # a bare `0` defaults to uint8 and solc rejects `int16 ? ... : 0` (branch-type mismatch).
    return "false" if ty == "bool" else f"{ty}(0)"


def gen_stateful_contract(seed):
    """A contract with state vars (scalar / mapping / dynamic array) + setters + read-modify-write
    mutators + getters, for fuzz_state.py's sequenced-state differ. Exercises the STORAGE codec
    (layout, packed sub-word, bytes32/bool/address scalars, mapping key derivation, array push/index)
    under read/write sequences. Arrays now include wide widths (uint128/int128/uint8/int16) since the
    wide-element .length stride bug is fixed (481f914810)."""
    rng = random.Random(seed)
    decls, fns = [], []
    for i in range(3):
        kind = rng.choice(["scalar", "mapping", "array"])
        if kind == "scalar":
            ty = rng.choice(STATE_SCALAR)
            decls.append(f"{ty} s{i};")
            fns.append(f"function set_s{i}({ty} v) external {{ s{i} = v; }}")
            if ty in STATE_NUM:
                op = rng.choice(["+", "-", "*", "|", "^", "&"])
                fns.append(f"function op_s{i}({ty} v) external {{ unchecked {{ s{i} = s{i} {op} v; }} }}")
            fns.append(f"function get_s{i}() external view returns ({ty}) {{ return s{i}; }}")
        elif kind == "mapping":
            ty = rng.choice(STATE_SCALAR)
            decls.append(f"mapping(uint256 => {ty}) m{i};")
            fns.append(f"function set_m{i}(uint256 k, {ty} v) external {{ m{i}[k] = v; }}")
            if ty in STATE_NUM:
                op = rng.choice(["+", "-", "*", "|", "^", "&"])
                fns.append(f"function op_m{i}(uint256 k, {ty} v) external {{ unchecked {{ m{i}[k] = m{i}[k] {op} v; }} }}")
            fns.append(f"function get_m{i}(uint256 k) external view returns ({ty}) {{ return m{i}[k]; }}")
        else:
            ty = rng.choice(STATE_ARRAY)
            decls.append(f"{ty}[] arr{i};")
            fns.append(f"function push_arr{i}({ty} v) external {{ arr{i}.push(v); }}")
            fns.append(f"function set_arr{i}(uint256 j, {ty} v) external {{ if (j < arr{i}.length) arr{i}[j] = v; }}")
            fns.append(f"function get_arr{i}(uint256 j) external view returns ({ty}) {{ return j < arr{i}.length ? arr{i}[j] : {_zero(ty)}; }}")
            fns.append(f"function len_arr{i}() external view returns (uint256) {{ return arr{i}.length; }}")
    body = "\n    ".join(decls) + "\n    " + "\n    ".join(fns)
    return "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.0;\ncontract G {\n    " + body + "\n}\n"


STRUCT_FIELD_TYPES = ["uint128", "int64", "uint8", "int16", "uint256", "bool", "address", "bytes32"]


def gen_struct_contract(seed):
    """A contract with a STORAGE struct of mixed-width fields + a whole-struct setter + per-field
    setters + per-field getters, for fuzz_state.py. Exercises the packed struct-field storage codec
    (sub-word field offsets, wide + bool + address fields, individual st.f writes vs whole-struct writes)."""
    rng = random.Random(seed)
    k = rng.randint(3, 4)
    fields = [(f"f{j}", ft) for j, ft in enumerate(rng.sample(STRUCT_FIELD_TYPES, k))]
    struct_def = "struct S { " + " ".join(f"{ft} {fn};" for fn, ft in fields) + " }"
    params = ", ".join(f"{ft} {fn}" for fn, ft in fields)
    args = ", ".join(fn for fn, _ in fields)
    fns = [f"function setAll({params}) external {{ st = S({args}); }}"]
    for fn, ft in fields:
        fns.append(f"function set_{fn}({ft} v) external {{ st.{fn} = v; }}")
        fns.append(f"function get_{fn}() external view returns ({ft}) {{ return st.{fn}; }}")
    body = "    " + struct_def + "\n    S st;\n    " + "\n    ".join(fns)
    return "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.0;\ncontract G {\n" + body + "\n}\n"


def gen_contract(seed, n_funcs, cf=False, arr=False, byts=False):
    rng = random.Random(seed)
    fns, src_by_sig = [], {}
    for i in range(n_funcs):
        ty = rng.choice(BYTES_TYPES if byts else TYPES)     # random width incl sub-word
        name = f"f{i}"
        if byts:                                            # bytesN bitwise/shift expressions
            expr = gen_bytes_expr(rng.randint(2, 4), rng, ty)
            body = f"return {expr};"
            params = f"{ty} a, {ty} b, {ty} c, {ty} d"
            sig = f"{name}({ty},{ty},{ty},{ty})"
        elif arr:                                           # array params + array-indexing loops
            ctr = [0]
            stmts = [gen_arr_stmt(rng, ty, ctr) for _ in range(rng.randint(2, 3))]
            inner = "\n        ".join(stmts)
            body = f"{ty} acc = a;\n        unchecked {{\n        {inner}\n        }}\n        return acc;"
            params = f"{ty}[] calldata arr, {ty}[][] calldata mat, {ty} a, {ty} b, {ty} c, {ty} d"
            sig = f"{name}({ty}[],{ty}[][],{ty},{ty},{ty},{ty})"
        elif cf:
            ctr = [0]
            stmts = [gen_stmt(rng.randint(1, 3), rng, ty, ctr) for _ in range(rng.randint(2, 4))]
            inner = "\n        ".join(stmts)
            chk = rng.random() < 0.5
            block = (f"unchecked {{\n        {inner}\n        }}" if chk else inner)
            body = f"{ty} acc = a;\n        {block}\n        return acc;"
            params = f"{ty} a, {ty} b, {ty} c, {ty} d"
            sig = f"{name}({ty},{ty},{ty},{ty})"
        else:
            expr = gen_expr(rng.randint(2, _DEPTH), rng, ty)
            body = f"return {expr};" if rng.random() < 0.5 else f"unchecked {{ return {expr}; }}"
            params = f"{ty} a, {ty} b, {ty} c, {ty} d"
            sig = f"{name}({ty},{ty},{ty},{ty})"
        fns.append(f"    function {name}({params}) external pure returns ({ty}) {{\n        {body}\n    }}")
        src_by_sig[sig] = f"{ty} {body[:200]}"
    src = "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.0;\ncontract G {\n" + "\n".join(fns) + "\n}\n"
    return src, src_by_sig


def main():
    argv = sys.argv[1:]

    def opt(flag, dflt):
        if flag in argv:
            i = argv.index(flag); return int(argv[i + 1])
        return dflt
    n_contracts, n_funcs = opt("--contracts", 5), opt("--funcs", 30)
    seed0, max_per_fn = opt("--seed", 1000), opt("--max-per-fn", 18)
    cf = "--cf" in argv                                     # control-flow bodies (loops/if/break/...)
    arr = "--arr" in argv                                   # array params + arr[i]/mat[i][j] in loops
    byts = "--bytes" in argv                                # bytesN bitwise/shift expressions
    global _CASTS, _DEPTH
    _CASTS = "--cast" in argv                               # round-trip casts ty->src->ty in expressions
    _DEPTH = opt("--depth", 4)                              # max expression-tree nesting depth
    if (cf or arr) and "--funcs" not in argv:
        n_funcs = 6                                         # CF/array bodies are verbose — fewer per 8KB app

    ln = LocalNet(); h = Harness(ln, HERE / "out_gen")
    base = {"solc_version": "0.8.26", "evm_version": "paris"}
    gen_path = HERE / "contracts" / "_gen.sol"

    total_calls = total_div = compile_fails = 0
    findings = []
    for seed in range(seed0, seed0 + n_contracts):
        src, src_by_sig = gen_contract(seed, n_funcs, cf, arr, byts)
        gen_path.write_text(src)
        b = {**base, "fixture": str(gen_path)}
        try:
            info = _oracle({**b, "introspect": True})
        except SystemExit as e:
            print(f"[seed {seed}] EVM introspect/compile failed: {str(e)[:120]}"); continue
        calls, _ = gen_calls(info["functions"], max_per_fn)
        outs_by_sig = {f["sig"]: f["outputs"] for f in info["functions"]}
        evm = _oracle({**b, "calls": [{"sig": s, "args": a} for s, a in calls]})["results"]
        try:
            app = h.compile_and_deploy(gen_path)
        except Exception as e:
            compile_fails += 1
            # Include the backend stderr tail so the crash REASON (e.g. the known
            # `'n' must be <= 255` optimizer fold crash) is in the log and the
            # classifier can auto-bucket it instead of needing a manual repro.
            reason = str(e)[:120]
            stderr = getattr(e, "stderr", "") or ""
            for cl in stderr.splitlines():
                if "critical:" in cl or "must be <= 255" in cl:
                    reason += " | " + cl.strip()[:160]
                    break
            findings.append((seed, "<contract>", "AVM COMPILE ERROR", reason, None, None))
            print(f"[seed {seed}] ⚠️  AVM compile error: {reason[:220]}"); continue

        sdiv = 0
        for (sig, args), er in zip(calls, evm):
            if er.get("ok"):
                expected = er["value"]
            elif er.get("revert"):
                expected = REVERT
            else:
                continue
            total_calls += 1
            try:
                r = h.call(app, sig, *args, expect_revert=True)
                actual = REVERT if r.reverted else r.abi_return
            except Exception:
                continue
            outs = outs_by_sig.get(sig, [])
            if canon(_apply_addr_canon(actual, outs)) != canon(_apply_addr_canon(expected, outs)):
                total_div += 1; sdiv += 1
                if sdiv <= 4:
                    findings.append((seed, sig, src_by_sig.get(sig, "?"), None, args, (expected, actual)))
        print(f"[seed {seed}] {len(calls)} calls, {sdiv} divergence(s)")

    print(f"\n=== {n_contracts} contracts, {total_calls} calls diffed | "
          f"{total_div} divergences, {compile_fails} AVM compile errors ===")
    for seed, sig, src, cerr, args, vals in findings[:40]:
        if cerr:
            print(f"\n❌ seed {seed} {sig}: {cerr}")
        else:
            exp, act = vals
            print(f"\n❌ seed {seed}  {sig}")
            print(f"     body: {src}")
            print(f"     {sig.split('(')[0]}{_fmt(args)}  evm={_fmt1(exp)}  avm={_fmt1(act)}")
    if not findings:
        print("\n✅ no divergences — generated codegen matches solc+EVM")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
