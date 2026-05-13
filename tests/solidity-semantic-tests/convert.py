"""Generate per-category test_<category>.py files from existing .sol fixtures.

Reads each .sol via the legacy parser, then emits a clean pytest module
where every test call is an explicit Python expression. The generated
file is the source of truth; the parser is only used here for one-shot
scaffolding.

Heuristics for `expected` decoding (from EVM-ABI words → Python):
  - Empty list / void          → assert result.abi_return is None
  - Single int (decimal/hex)   → assert result.abi_return == int
  - Single bool                → assert result.abi_return is bool
  - Multi-int tuple (no `0x`,
    no quoted strings)         → assert tuple(...) == (int, int, ...)
  - Solidity revert pattern    → expect_revert=True
  - Anything else              → emit a `# TODO` line and assert that the
    call didn't revert (so the test still pings the compiler/harness
    but the structural shape is left for a human pass).

Generated tests preserve the original .sol filename → test function name.
"""
from __future__ import annotations

import re
from pathlib import Path
from textwrap import indent

from parser import parse_test_file, parse_value, TestCall, SemanticTest


SOL_TO_ARC4_INNER = {
    "bool": "bool",
    "address": "address",
    "string": "string",
    "bytes": "byte[]",
}
for _n in range(1, 33):
    SOL_TO_ARC4_INNER[f"bytes{_n}"] = f"byte[{_n}]"
for _n in (8, 16, 32, 64, 128, 256):
    SOL_TO_ARC4_INNER[f"uint{_n}"] = f"uint{_n}"
    SOL_TO_ARC4_INNER[f"int{_n}"] = f"int{_n}"


def _py_value_for_arg(raw: str) -> str:
    """Translate a parsed test-call arg (still a string) into a Python literal."""
    v = parse_value(raw)
    if v is None:
        return "None"
    if isinstance(v, bool):
        return "True" if v else "False"
    if isinstance(v, int):
        if v > 2**32:
            return f"0x{v:x}"
        return str(v)
    if isinstance(v, bytes):
        if len(v) <= 32:
            return f"bytes.fromhex({v.hex()!r})"
        return f"bytes.fromhex({v.hex()!r})  # {len(v)} bytes"
    if isinstance(v, str):
        return repr(v)
    return repr(raw)


def _simple_int_expected(expected: list[str]) -> int | None:
    """If expected is a single decimal/hex int, return it; else None."""
    if len(expected) != 1:
        return None
    e = expected[0].strip()
    if not e:
        return None
    if e in ("true", "false"):
        return None
    if e.startswith("right(") and e.endswith(")"):
        return _simple_int_expected([e[6:-1]])
    if e.startswith("-"):
        try:
            return int(e)
        except ValueError:
            return None
    if e.startswith("0x"):
        try:
            return int(e, 16)
        except ValueError:
            return None
    try:
        return int(e)
    except ValueError:
        return None


def _simple_bool_expected(expected: list[str]) -> bool | None:
    if len(expected) != 1:
        return None
    e = expected[0].strip()
    if e == "true":
        return True
    if e == "false":
        return False
    if e == "right(true)":
        return True
    if e == "right(false)":
        return False
    return None


def _all_simple_ints(expected: list[str]) -> list[int] | None:
    """If every expected value is a plain int/hex, return them; else None."""
    out = []
    for e in expected:
        e = e.strip()
        if not e:
            return None
        n = _simple_int_expected([e])
        if n is None:
            return None
        out.append(n)
    return out


def _all_simple_bools(expected: list[str]) -> list[bool] | None:
    out = []
    for e in expected:
        e = e.strip()
        b = _simple_bool_expected([e])
        if b is None:
            return None
        out.append(b)
    return out


def _arg_list_expr(args: list[str]) -> str:
    """Return a comma-joined Python-arg list, e.g., '3, True, b\"...\"' """
    return ", ".join(_py_value_for_arg(a) for a in args)


_PARAM_TYPES_RE = re.compile(r"^[a-zA-Z_]\w*\(([^)]*)\)$")


def _method_param_types(sig: str) -> list[str] | None:
    """Extract the comma-separated param type list from a Solidity-style sig.

    Returns ['string'] for 'f(string)', ['uint256','uint256'] for 'g(u,u)',
    or None if the signature can't be parsed (e.g., nested tuples).
    """
    m = _PARAM_TYPES_RE.match(sig)
    if not m:
        return None
    inner = m.group(1)
    if not inner:
        return []
    if "(" in inner:  # nested tuples — skip
        return None
    return [t.strip() for t in inner.split(",")]


def _reassemble_dynamic_arg(args: list[str]) -> str | None:
    """If `args` is `[offset, length, data]` and data is a single dynamic
    scalar, return a single Python literal for it. Else None.

    Recognises:
      [32, len, "string"]    → "string"
      [32, len, hex"abcd..."] → bytes
    """
    if len(args) != 3:
        return None
    off = args[0].strip()
    if off not in ("32", "0x20"):
        return None
    length_raw = args[1].strip()
    try:
        length = int(length_raw, 16) if length_raw.startswith("0x") else int(length_raw)
    except ValueError:
        return None
    data = args[2].strip()
    if data.startswith('"') and data.endswith('"'):
        # String content — strip quotes, decode escapes, truncate to length.
        s = parse_value(data)
        if isinstance(s, bytes):
            s = s[:length].decode("utf-8", errors="replace")
            return repr(s)
        return None
    if data.startswith('hex"') and data.endswith('"'):
        b = parse_value(data)
        if isinstance(b, bytes):
            b = b[:length] if length <= len(b) else b
            return f"bytes.fromhex({b.hex()!r})"
        return None
    return None


_PY_KEYWORDS = {
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield", "match", "case",
}


def _sanitize_function_name(stem: str) -> str:
    """Map a .sol filename stem to a valid Python identifier."""
    name = re.sub(r"[^a-zA-Z0-9_]", "_", stem)
    if name and name[0].isdigit():
        name = "_" + name
    if name in _PY_KEYWORDS:
        name = name + "_"
    return name


def _render_call(call: TestCall, category: str, sol_stem: str) -> list[str]:
    """Render one TestCall as Python source lines (without indentation)."""
    sig = call.method_signature

    # Dynamic-arg reassembly: f(string): 32, 16, "..." → f(string), "...".
    # The Solidity isoltest format passes the raw EVM-encoded calldata as
    # three top-level values; algokit / algosdk wants a single typed value.
    param_types = _method_param_types(sig)
    if (
        param_types
        and len(param_types) == 1
        and param_types[0] in ("string", "bytes")
    ):
        reassembled = _reassemble_dynamic_arg(call.args)
        if reassembled is not None:
            args_expr = reassembled
        else:
            args_expr = _arg_list_expr(call.args)
    else:
        args_expr = _arg_list_expr(call.args)

    payment = f", payment_wei={call.value_wei}" if call.value_wei else ""

    lines: list[str] = []
    if call.expect_failure:
        lines.append(f"# {call.raw_line}")
        callargs = f'"{sig}"' + (f", {args_expr}" if args_expr else "")
        lines.append(
            f"r = harness.call(app, {callargs}{payment}, expect_revert=True)"
        )
        lines.append("assert r.reverted")
        return lines

    if sig == "()":
        # Bare fallback call; we don't have ABI dispatch — skip with xfail.
        lines.append(f"# {call.raw_line}")
        lines.append('pytest.xfail("fallback() dispatch not yet implemented")')
        return lines

    # Build the call expression
    callargs = f'"{sig}"' + (f", {args_expr}" if args_expr else "")
    lines.append(f"# {call.raw_line}")
    lines.append(f"r = harness.call(app, {callargs}{payment})")

    expected = [e for e in call.expected if e.strip() != ""]
    if not expected:
        lines.append("# (void return — call succeeding is the assertion)")
        return lines

    # Simple cases:
    si = _simple_int_expected(expected)
    if si is not None:
        lines.append(f"assert r.abi_return == {si}")
        return lines
    sb = _simple_bool_expected(expected)
    if sb is not None:
        lines.append(f"assert r.abi_return is {sb}")
        return lines

    bools = _all_simple_bools(expected)
    if bools is not None:
        lines.append(f"assert tuple(r.abi_return) == {tuple(bools)!r}")
        return lines

    ints = _all_simple_ints(expected)
    if ints is not None:
        if 2 <= len(ints) <= 4:
            # Most 2-4 int returns are flat tuples from multi-return functions.
            # If the actual return is dynamic-encoded (e.g. an array or
            # string), the assertion will fail loudly and the human can
            # rewrite it to a structural check.
            lines.append(f"assert tuple(r.abi_return) == {tuple(ints)!r}")
            return lines
        joined = ", ".join(str(i) for i in ints)
        lines.append(f"# TODO: verify structural decoding matches expected: {joined}")
        lines.append("assert not r.reverted")
        return lines

    # String return pattern: [0x20, length, "any"] → returns "any"
    if (
        len(expected) == 3
        and expected[0].strip() in ("0x20", "32")
        and expected[2].strip().startswith('"')
        and expected[2].strip().endswith('"')
    ):
        sval = expected[2].strip()[1:-1]
        lines.append(f"assert r.abi_return == {sval!r}")
        return lines

    # Anything else — leave a TODO
    lines.append(f"# TODO: verify expected: {' | '.join(expected)}")
    lines.append("assert not r.reverted")
    return lines


def render_test_file(category: str, sol_files: list[Path]) -> str:
    """Render the full test_<category>.py source."""
    header = [
        '"""Auto-generated tests for the {category} category.',
        '',
        'Each test deploys the contract defined in the matching .sol file and',
        'runs the assertions originally documented in the test\'s `// ----`',
        'block. The .sol files are unchanged; this Python module is the new',
        'source of truth — edit it freely to fix or sharpen assertions.',
        '"""',
        'import pytest',
        '',
        'from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted',
        '',
        '',
    ]
    header[0] = header[0].format(category=category)

    body: list[str] = []
    for sol in sorted(sol_files):
        stem = sol.stem
        fname = "test_" + _sanitize_function_name(stem)
        test = parse_test_file(sol)
        rel = f"{category}/{sol.name}"

        body.append(f"def {fname}(harness):")
        body.append(f'    """{rel}"""')

        compile_opts = []
        if test.compile_via_yul:
            compile_opts.append("via_yul_behavior=True")
        if test.evm_version:
            compile_opts.append(f'evm_version={test.evm_version!r}')
        compile_kwargs = (", " + ", ".join(compile_opts)) if compile_opts else ""

        # Detect constructor() with args + value_wei in the first call
        ctor_extra = ""
        first_calls = list(test.calls)
        if first_calls and first_calls[0].method_signature.startswith("constructor("):
            c = first_calls[0]
            if c.args:
                ctor_extra += f", ctor_args=[{_arg_list_expr(c.args)}]"
            if c.value_wei:
                ctor_extra += f", fund_wei={c.value_wei}"
            first_calls = first_calls[1:]

        body.append(
            f'    app = harness.compile_and_deploy("{rel}"{compile_kwargs}{ctor_extra})'
        )

        if not first_calls:
            if test.calls:
                body.append("    # constructor-only test — deployment succeeding is the assertion")
            else:
                body.append("    # no assertions in source — deployment succeeding is the assertion")
            body.append("")
            continue

        for c in first_calls:
            if c.method_signature == "__xfail_isoltest_bareword__":
                body.append('    pytest.xfail("isoltest-bareword test — no AVM analogue")')
                continue
            for line in _render_call(c, category, stem):
                body.append("    " + line)

        body.append("")  # blank line between tests
    return "\n".join(header + body)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--category", help="Only convert this category (default: all)")
    ap.add_argument("--dry-run", action="store_true", help="Print to stdout instead of writing")
    args = ap.parse_args()

    tests_dir = Path(__file__).parent / "tests"
    categories = sorted([d for d in tests_dir.iterdir() if d.is_dir()])
    if args.category:
        categories = [d for d in categories if d.name == args.category]
    if not categories:
        print("No categories matched.")
        return

    summary = []
    for cat_dir in categories:
        sol_files = sorted(cat_dir.glob("*.sol"))
        if not sol_files:
            continue
        out_path = cat_dir / f"test_{_sanitize_function_name(cat_dir.name)}.py"
        # Skip if a hand-written test file already exists for this category
        # (smoke/test_smoke.py is the gold standard — don't overwrite).
        if out_path.exists() and out_path.stat().st_size > 0:
            existing = out_path.read_text()
            if "Auto-generated" not in existing:
                summary.append(f"{cat_dir.name}: SKIPPED (hand-written file present)")
                continue
        src = render_test_file(cat_dir.name, sol_files)
        if args.dry_run:
            print(f"===== {cat_dir.name} ({len(sol_files)} fixtures) =====")
            print(src[:2000])
        else:
            out_path.write_text(src)
            summary.append(f"{cat_dir.name}: wrote {out_path.name} ({len(sol_files)} fixtures)")

    for line in summary:
        print(line)
    print(f"\nGenerated test files for {len(summary)} categories.")


if __name__ == "__main__":
    main()
