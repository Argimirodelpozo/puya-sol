# Semantic Test Framework Refactor

## What changed

The old infrastructure (`run_tests.py`, the old `conftest.py`, `parser.py`)
parsed Solidity isoltest `// ----` assertion blocks at runtime and tried
to match raw EVM ABI words against algosdk's decoded return values.
Result: ~3,000 lines of comparison logic, fragile multi-line parsing,
silent "vacuous passes" when the parser missed an assertion, and
expectations that were impossible to read at a glance.

The new structure replaces the runtime parser with **explicit Python
test functions**, one per `.sol` fixture. Expectations live next to the
call site as typed Python values. The parser still exists as a one-shot
codegen tool (`convert.py`), but its output is the source of truth from
that point on — you edit the Python, not the parser.

## Layout

```
tests/solidity-semantic-tests/
├── framework/                ← Shared library
│   ├── __init__.py           — public API: Harness, lpad/rpad, revert classifiers
│   ├── _algosdk_patch.py     — teach algosdk about signed int<N> types
│   ├── paths.py              — COMPILER, PUYA, TESTS_DIR, OUT_DIR
│   ├── localnet.py           — session LocalNet wrapper
│   ├── compile.py            — puya-sol subprocess → CompiledArtifacts
│   ├── deploy.py             — deploy + fund + __postInit
│   ├── call.py               — happy-path call + simulate-based revert capture
│   ├── values.py             — lpad/rpad/u256/i256 helpers
│   ├── revert.py             — Reverted / ErrorString / Panic / RawRevert markers
│   └── harness.py            — Harness class (compile_and_deploy + call)
├── conftest.py               ← Pytest fixtures (localnet, harness)
├── convert.py                ← Codegen — reads .sol via legacy parser, emits test_<cat>.py
├── parser.py                 ← Legacy parser (codegen-only; not imported by tests)
├── multisource_splitter.py   ← Multi-source fixture splitter (still used by framework.compile)
├── tests/
│   ├── <category>/
│   │   ├── *.sol             ← Unchanged Solidity fixtures
│   │   └── test_<category>.py ← Generated or hand-written pytest module
│   └── smoke/test_smoke.py   ← Hand-written reference category
└── legacy/                   ← Old run_tests.py, old conftest, analyzers — kept for grep
```

## Running

```bash
# Everything
pytest tests/

# One category
pytest tests/modifiers

# One test
pytest tests/modifiers/test_modifiers.py::test_function_modifier_library

# Parallel (requires pytest-xdist)
pytest tests/ -n 8

# Filter by category marker
pytest tests/ -m cat_smoke
```

## Writing a test

Every test takes the `harness` fixture and orchestrates compile + deploy +
call manually. The framework exposes:

```python
harness.compile_and_deploy(sol_path, contract_name=None,
                            *, ctor_args=None, fund_wei=0,
                            evm_version=None, via_yul_behavior=False,
                            ensure_budget=None) → App

harness.call(app, "sig(types)", *args,
             *, payment_wei=0, expect_revert=False,
             extra_fee=0, ...) → Result

harness.call_raw(app, selector: bytes,
                 *, extra_args=(), payment_wei=0,
                 expect_revert=False, ...) → Result
```

`Result` is a dataclass:

```python
Result(
    abi_return: Any,          # decoded return value (algosdk gives us typed values)
    logs: list[bytes],         # Solidity events (when emitted via log opcode)
    reverted: bool,
    revert_data: bytes,        # raw revert bytes (from puya-sol log emission)
    revert_reason: Reverted | ErrorString | Panic | RawRevert,
    fail_message: str,
)
```

### Example: simple value-equality

```python
def test_basic(harness):
    app = harness.compile_and_deploy("smoke/basic.sol")
    assert harness.call(app, "f(uint256)", 3).abi_return == (3, 3)
    assert harness.call(app, "i(bool)", True).abi_return is False
```

### Example: payment + revert classification

```python
def test_failure(harness):
    app = harness.compile_and_deploy("smoke/failure.sol")
    r = harness.call(app, "e()", expect_revert=True)
    assert r.reverted
    # When puya-sol emits Error(string) bytes:
    # assert r.revert_reason == ErrorString("Transaction failed.")
```

### Example: constructor with value forwarding

```python
def test_constructor(harness):
    app = harness.compile_and_deploy("smoke/constructor.sol",
                                      ctor_args=[3], fund_wei=2)
    assert harness.call(app, "state()").abi_return == 3
```

## Adding a quirk to a single test

There's no central "quirks" table — each test owns its own logic. If a
contract needs:
- a non-default EVM version → pass `evm_version=...`
- extra opcode budget → pass `extra_fee=2000` to the call
- a specific contract from a multi-contract file → pass `contract_name=`
- raw calldata that bypasses ABI dispatch → use `harness.call_raw(...)`

If the auto-generated assertion is wrong, edit the Python. Don't refactor
the framework to handle one weird case — keep the framework boring and
the per-test code expressive.

## Codegen status

`convert.py` reads each `.sol` via the legacy parser and emits a Python
function per fixture. Heuristics:

| Expected shape                  | Emitted assertion                      |
|---------------------------------|----------------------------------------|
| empty (`->`)                    | (none — call succeeding is the test)   |
| single int / hex                | `assert r.abi_return == N`             |
| single `true` / `false`         | `assert r.abi_return is BOOL`          |
| 2–4 simple ints                 | `assert tuple(r.abi_return) == (...)`  |
| `[0x20, len, "string"]`         | `assert r.abi_return == "string"`      |
| `FAILURE, ...`                  | `expect_revert=True; assert r.reverted`|
| anything else                   | `# TODO: ...` + `assert not r.reverted`|

Known under-coverage in codegen output:
- `byte[N]` returns get compared as `int` — fails because algosdk returns
  `list[int]`. Hand-fix: `bytes(r.abi_return) == lpad(value, N)`.
- Dynamic single-arg encoding (`f(string): 32, 16, hex"..."`) is passed
  as three Python args. Hand-fix: reassemble into one `str` / `bytes`.
- Multi-return shapes get a `# TODO` because heuristics can't tell a
  flat tuple from a structural EVM encoding.

The codegen is one-shot. Once a test file is hand-edited, regenerating
will skip it iff it doesn't have the `"Auto-generated"` marker in the
docstring. To force regenerate, delete the file first.

## Migration status

See `BASELINE.md` (generated by the first full pytest run) for the
authoritative pass/fail/xfail counts per category. The mental model:

- **Pass** — the test runs and the assertions hold. Framework + compiler + test wiring all aligned.
- **Fail** — at least one assertion missed. Either the codegen guessed wrong
  (most common; hand-edit) or there's a real compiler/harness gap.
- **xfail** — explicitly known-broken (e.g. `fallback()` dispatch not implemented).
- **Error** — the harness itself blew up (compile error, deploy error, fixture failure).

The migration backlog is the set of failing tests. Each one is a small,
local fix in a Python file — no parser to coax, no comparison logic to
trace through.
