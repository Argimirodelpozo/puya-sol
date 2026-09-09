# Solidity semantic tests

The suite combines ported upstream Solidity semantic fixtures with puya-sol
regressions. Explicit Python assertions in `tests/<category>/test_*.py` are the
test oracle; Solidity `// ----` comments are not parsed at runtime. The old
parser and analysis scripts remain under `legacy/` for historical reference,
not as an active test runner.

## Recorded baseline

Full LocalNet semantic and harness/cache run on **2026-09-07**. The tested
compiler and regression sources are committed as `9521c807ba` (rev-2 after
default-off proxy adaptation, solc-directed modifier lookup, pointer-backed
modifier memory parameters, and the AWST construction cleanup):

| Result | Count |
|---|---:|
| Passed | 1,860 |
| Failed | 1 |
| Expected failure (xfail) | 101 |
| Unexpected pass (xpass) | 38 |
| Total | 2,000 |

The run took 449.42 seconds with three workers and a warm compilation cache.
Dependencies were pinned to Solidity
`a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d` and Puya
`27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1). Native CTest coverage
passed 19/19. Harness/cache unit tests are included in the full count. The six
new ordinary passes cover legacy/via-IR modifier lookup and default-off proxy
behavior in both storage layouts. The local JUnit report is
`/tmp/puyasol-rev-2-current-semantic.xml`. The compiler stayed fixed during the
run, and LocalNet was not reset. See the
[completed sol-types/storage audit record](../../docs/rev-2-results.md) for its
earlier binary identity, solc oracle settings and fresh-deployment-only format
changes.
These are results for that revision and local environment, not a guarantee
about future commits or arbitrary contracts. Xpasses are non-strict in this
run and are not folded into the ordinary pass count. Existing XPASS/xfail
markers were left unchanged; their review remains deferred.

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

### Working-tree verification — 2026-09-08

The [Yul subroutine implementation](../../docs/yul-subroutines.md) and
[memory-word/call-fact follow-up](../../docs/memory-word-subroutines.md), on `rev-2`
base `5dee40fa5a0e8794f4c416729e4b34ed20d17c73` with uncommitted changes, was
tested with compiler SHA-256
`914afa3dce0c73902cd4e800877d68c31cee9ddd00468bbad1236d61c12ae34a`:
**1,880 passed, 1 failed, 101 xfailed, 38 xpassed** (2,020 total), in 416.38
seconds with three workers. No new failures were observed; the sole failure
is the same Puya DCE/divide-by-zero bug described above. Marker review remains
deferred. Native CTests passed 19/19. The compiler was held fixed and LocalNet
reset was disabled. The report is
`/tmp/puya-sol-memory-size-20260908.3MgBFL/semantic-final.xml`.
All 2,014 cases from the outlining run retain their JUnit outcome; the six
additional memory/call-fact cases pass. Relative to the committed 2,000-case
baseline, there are twelve new Yul/memory cases and eight harness/cache tests;
no baseline tests were removed.

### Call-boundary refactor verification — 2026-09-09

The five-refactor batch on base `ddda7255dd6358abf1b8d6d0531043a3377b9653`
was tested with compiler SHA-256
`63bcbbb8993d1717f104fd3dd26179e69630b8c89f740029e2deb1bf02fd7fd1`.
It replaces the mutable function-pointer initializer cache with runtime reads
and specialization only for source initializers proven stable by solc write
facts; unifies pointer argument sequencing, source-function context construction,
and native/wire return adaptation; and removes redundant context APIs and scans.
The `src/` change is 356 lines added and 1,114 removed (net **−758**).
The existing memory representation is unchanged.

Full verification used `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3
--tb=short --junitxml=/tmp/puya-sol-call-refactors-20260909.0xqSOC/semantic-final.xml`:
**1,918 passed, 1 failed, 101 xfailed, 38 xpassed** (2,058 total), in 352.00
seconds. All 2,044 cases from the preceding scope/super-call run retained their
individual JUnit outcomes, and all 14 new cases passed. The sole failure remains
the Puya DCE/divide-by-zero bug above; markers were not changed. The compiler
hash stayed fixed, LocalNet was not reset, and dependency pins are unchanged.

[The new regressions](tests/puyasolRegression/test_call_boundaries.py) cover
pointer reassignment, tuple swaps, loop re-entry, deletion, assembly writes,
state/field/index reads, callee-versus-argument evaluation order, reference
write-back for stable pointers, free/library/contract calldata contexts,
signed and tuple returns, keyed self getters, and real cross-contract pointer
calls under ARC-4 and EVM ABI profiles. Expected results were checked independently
against solc 0.8.34 on PyEVM: 29 checks each with legacy and via-IR compilation,
optimizer enabled at 200 runs, EVM version Cancun (58/58 passed). This includes
the different internal-pointer callee/argument order emitted by the two solc modes.
Focused call/return tests passed 49/49, native CTests 19/19, and chainwide harness
unit tests 91/91. JUnit reports and the outcome comparison are retained in
`/tmp/puya-sol-call-refactors-20260909.0xqSOC/`.

### Commands

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
PUYASOL_LOCALNET_RESET=0 pytest tests/puyasolRegression/test_native_payments.py -q -n 2
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
These generated directories and historical run logs are untracked and ignored;
the harness still mirrors current artifacts for diagnostics and tests that
inspect compiler output. Untracking preserved existing local files and did not
rewrite Git history. Intentional source fixtures remain tracked. Record future
results with the tested commit, dependency revisions, command, and a retained
JUnit artifact outside the checkout.

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
