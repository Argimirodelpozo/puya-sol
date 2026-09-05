# Solidity semantic tests

The suite combines ported upstream Solidity semantic fixtures with puya-sol
regressions. Explicit Python assertions in `tests/<category>/test_*.py` are the
test oracle; Solidity `// ----` comments are not parsed at runtime. The old
parser and analysis scripts remain under `legacy/` for historical reference,
not as an active test runner.

## Recorded baseline

Full LocalNet run on **2026-09-05**, compiler and tests committed as
`d3668ac0ff` (static-call warning follow-up to `9d74611bb0`):

| Result | Count |
|---|---:|
| Passed | 1,623 |
| Failed | 1 |
| Expected failure (xfail) | 101 |
| Unexpected pass (xpass) | 39 |
| Total | 1,764 |

The run took 1,071.96 seconds with two workers. Dependencies were pinned to
Solidity `a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d` and Puya
`27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1). Native CTest coverage passed
16/16; the focused static-call and builder regression selection passed 42/42.
These are results for that revision and local environment, not a guarantee
about future commits or arbitrary contracts. Xpasses are non-strict in this
run and need review; they are not folded into the ordinary pass count.

The remaining failure is
`puyasolRegression/test_puyasol_regression.py::test_dce_reverting_subexpr_literal_folds`:
`divdivShl(uint256)(0)` returns zero instead of reverting. The pinned Puya
optimizer can discard an unused division/modulo expression even when it must
trap on a zero divisor. This is an open backend bug, **not** an accepted AVM
divergence, and the regression remains a normal failing test.

Cross-contract static-call read-only enforcement **is** an accepted divergence.
The compiler warns, and `various/test_various.py::test_staticcall_for_view_and_pure`
asserts the accepted AVM behavior. See [the divergence policy](../../EVM_DIVERGENCE.md).

## Running

Build the frontend and set up the pinned Puya environment as described in the
[root README](../../README.md). The test interpreter needs `pytest`,
`pytest-xdist`, `algokit-utils`, and `py-algorand-sdk`; some categories also use
cryptographic packages such as `pycryptodome` and `eth-keys`. The test environment
does not yet have a complete pinned dependency manifest. Runtime tests require
an already-running AlgoKit LocalNet with algod and KMD available.

From this directory:

```bash
# Full run; retain a machine-readable report outside the checkout.
PUYASOL_LOCALNET_RESET=0 pytest tests/ -q -n 2 --tb=short \
  --junitxml=/tmp/puyasol-semantic.xml

# Focused compiler regressions.
PUYASOL_LOCALNET_RESET=0 pytest tests/puyasolRegression/test_builder_findings.py -q -n 2
PUYASOL_LOCALNET_RESET=0 pytest tests/smoke/ -q

# Harness/cache units, without deploying contracts.
PUYASOL_LOCALNET_RESET=0 pytest framework/test_compile_cache.py framework/test_harness.py -q
```

Always set `PUYASOL_LOCALNET_RESET=0` when preserving the current ledger matters.
Without it, the collection hook can automatically reset an aged LocalNet for a
large run, deleting its apps and state. Do not rebuild or replace the compiler
or backend while a suite is running.

The harness defaults to `build/puya-sol` and `puya/.venv/bin/puya`. Alternate
builds can be selected with `PUYA_SOL_COMPILER`, `PUYA_SOL_PUYA`, and
`PUYA_SOL_PUYA_SRC`; see [framework/paths.py](framework/paths.py). It explicitly
enables legacy source rewriting and eligible divergence policies for corpus
research. This differs from an ordinary compiler invocation.

Compile caches live in `.compile_cache/`, with per-test artifacts under `out/`.
Some legacy output files are still tracked: inspect Git status after a run and
do not commit incidental regenerated artifacts. Historical result snapshots and
the old append-only status report were removed; their tracked versions remain
available in Git history. Record future results with the tested commit,
dependency revisions, command, and a retained JUnit artifact.

## Adding tests

Place fixtures under `tests/<category>/contracts/` and add explicit assertions
in that category's Python module. Relative fixture paths are resolved against
`tests/`, so include the `contracts/` component:

```python
def test_basic(harness):
    app = harness.compile_and_deploy("smoke/contracts/basic.sol")
    result = harness.call(app, "f(uint256)", 3)
    assert tuple(result.abi_return) == (3, 3)
```

Use `contract_name=` for a particular contract, `ctor_args=` for constructor
arguments, and `extra_args=` for compiler options. `harness.call_raw` exercises
raw selectors; `harness.call_bare` exercises receive/fallback routing. With
`expect_revert=True`, inspect `result.reverted` explicitly: merely allowing a
revert is not an assertion that one occurred. Keep compiler/backend defects
distinct from documented platform divergences; do not weaken an assertion or
add an xfail solely to make a run green. See [the smoke tests](tests/smoke/test_smoke.py)
and [the harness API](framework/harness.py) for working examples.
