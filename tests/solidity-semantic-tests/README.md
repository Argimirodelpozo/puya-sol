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

### Block-lowering refactor verification — 2026-09-09

The block refactors on base `7bf4febee32fcca6a14f154016eb86c54166b321`
were tested with compiler SHA-256
`e8d4ac733828c6ba9c0a6124fbd5f70f51c85c866c7ffeb51ba7702a49648021`.
Block and brace-less bodies now share one child-scope builder; source lowering
and loop fallthrough use the existing AWST termination predicate; and `try`
success bindings use solc's success clause and checked declaration types, with
the shared call-result adapter handling physical return representations.
The redundant `SolBlock` class/header and assembly-only block termination flag
are removed. The `src/` change is 80 lines added and 256 removed (net **−176**).
Loop continuation representation and the custom AST visitor are unchanged.

Full verification used `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3
--tb=short --junitxml=/tmp/puya-sol-blocks-20260909.4iukEf/semantic-final.xml`:
**1,930 passed, 1 failed, 101 xfailed, 38 xpassed** (2,070 total), in 274.13
seconds. All 2,058 preceding cases retained their individual JUnit outcomes,
and all 12 new cases passed. This is not a fully green suite: the sole failure
remains the open Puya DCE/divide-by-zero bug above, not an accepted divergence.
Markers were unchanged. The compiler hash stayed fixed, dependency pins are
unchanged, and LocalNet was not reset.

The saved pre-change compiler was also rerun on these fixtures: the four block
cases passed, all eight `try` configurations failed compilation, and the same
DCE/divide-by-zero failure reproduced (`baseline-focused.xml`). The final
compiler passed the focused block/try rerun after this comparison.

[The new regressions](tests/puyasolRegression/test_blocks.py) cover lexical
shadowing and unchecked restoration, deep returns/reverts, one- and two-sided
halts, nested loops, break/continue and brace-less bodies; and signed, tuple,
unnamed, omitted, void and nested `try` success bindings. Real cross-contract
calls exercise ARC-4/EVM ABI profiles, both storage layouts, and legacy/via-IR
behavior, including the warning and transaction rollback for accepted AVM
catch-abort behavior. Expected results were independently checked with solc
0.8.34 on PyEVM, optimizer enabled at 200 runs and EVM version Cancun:
32 checks per mode, **64/64 passed**, explicitly distinguishing EVM catch
recovery from AVM aborts.

Focused block/try tests passed 13/13, native CTests 19/19, AVM standard-library
runtime tests 20/20, and chainwide harness unit tests 91/91. JUnit reports,
the solc oracle script, and the case-by-case comparison are retained locally
in `/tmp/puya-sol-blocks-20260909.4iukEf/`.

### Loop continuation and solc visitor verification — 2026-09-09

The follow-up to the block batch above, still on `rev-2` base
`7bf4febee32fcca6a14f154016eb86c54166b321` with uncommitted changes, was
built with compiler SHA-256
`a1b44c1cc40f41d5aff6f91e0ead3a82258a839847e84d788462b153dde766b3`.
For-post and do/while continuation handling now use one factory, lowering the
solc source afresh at each continue and fallthrough site. This avoids sharing
mutable AWST and `SingleEvaluation` identities between independent sites. A
scope guard preserves the loop's own checked/unchecked state even when a
continue is nested in an unchecked block.

Both expression and statement dispatchers now use solc's `ASTConstVisitor` and
`accept`; each handler disables automatic child traversal and retains explicit
child evaluation order. Unhandled nodes still report an error. The redundant
112-line `SolASTVisitor.h` is removed, without introducing another visitor
adapter. These two refactors remove a further **109 net lines from `src/`**;
the combined five-refactor change is 252 lines added and 537 removed (**−285**).
The memory representation is unchanged.

[The loop/visitor regressions](tests/puyasolRegression/test_loop_visitors.py)
cover two continue sites plus fallthrough, break paths, nested loops,
memory/state side effects, lexical checkedness, expression evaluation order,
conditional/short-circuit gating, and calldata slices. Structural assertions
also require nonempty, disjoint `SingleEvaluation` ID sets for all three prefix
expansions. The saved pre-refactor compiler fails all four loop configurations
at runtime on these fixtures; the new compiler passes them. The four scoped
visitor cases pass before and after, confirming preservation of dispatch.

Full verification used `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3
--tb=short --junitxml=/tmp/puya-sol-loops-visitors-20260909.yXtvB9/semantic-final.xml`:
**1,938 passed, 1 failed, 101 xfailed, 38 xpassed** (2,078 total), in 233.53
seconds. Every one of the preceding 2,070 cases retained its JUnit outcome;
all eight new cases passed. The existing Puya DCE/divide-by-zero failure
remains a normal failure, not an accepted divergence. No markers changed.
The compiler stayed fixed during testing, dependency pins are unchanged,
and LocalNet reset remained disabled.

Focused tests passed 21/21, native CTests 19/19, AVM standard-library runtime
tests 20/20, and chainwide harness unit tests 91/91. Independent solc 0.8.34 /
PyEVM checks passed 60/60 (30 each in legacy and via-IR mode, optimizer 200 runs,
EVM version Cancun). The expression trace differs between solc's two modes,
and the test asserts the appropriate reference trace for each.

#### Separate pre-existing bugs exposed by the initial probes

Two independent issues also reproduce with the saved pre-refactor compiler
(`e8d4ac733828c6ba9c0a6124fbd5f70f51c85c866c7ffeb51ba7702a49648021`):

- General assignment sequencing in via-IR mode: when both an array index and
  the RHS call `mark`, the compiler emits the LHS effect first, whereas solc
  0.8.34 executes the RHS first. In the original visitor fixture,
  `expressions(false)` produces trace `15679` instead of solc's `51679` in
  both storage layouts. For this refactor-only verification, the scoped visitor
  test materialized the RHS in a separate statement to isolate dispatch.
- Using a memory-array increment directly as a value, e.g.
  `return ++state[0] < limit` for `uint64[] memory state`, fails compilation.
  The legacy probe reports an ARC-4 uint64 where a native comparison operand
  is required; the via-IR probe reports an assignment target/value mismatch.
  The scoped loop fixture incremented and read in separate statements,
  preserving its state effects while isolating the continuation refactor.

Neither issue was fixed or reclassified by that refactor batch; both are
addressed by the follow-up below, which restores the original failing forms.
Initial reproducers, their solc results, the before/after JUnit reports, and
the final verification artifacts are retained locally in
`/tmp/puya-sol-loops-visitors-20260909.yXtvB9/`.

### Assignment ordering and increment values — 2026-09-09

Both code generators in the pinned solc source (`ExpressionCompiler::visit`
and `IRGeneratorForStatements::visit` for `Assignment`) evaluate the RHS fully
before the LHS, including compound assignments. The generic assignment path
now follows that order in both behavior modes. LHS index-call write-backs also
finish before the final read/store: previously a memory-array write-back could
overwrite the assignment itself. Existing operand-effect helpers implement
both boundaries without another sequencing abstraction.

Prefix increment/decrement now preserves the AWST assignment's physical target
type and decodes an ARC-4 result to the native type mapped from solc's unary
expression annotation. Overriding the assignment's reported type was invalid:
Puya derives it from the target. Arithmetic width, signedness, checked overflow,
and postfix behavior continue to use the existing lowering.

[Dedicated regressions](tests/puyasolRegression/test_assignment_incdec.py) cover
plain/compound assignment, RHS and LHS memory mutations, state-index changes,
single-evaluation indexes, prefix/postfix increment and decrement, unsigned
8/64/128-bit values, signed 8-bit boundaries, and checked versus wrapping
arithmetic. The original loop comparison and inline indexed assignment are
restored in the loop/visitor fixtures. Independent solc 0.8.34/PyEVM checks
pass **216/216** (108 per mode; optimizer 200 runs, Cancun).

The saved pre-fix compiler
(`a1b44c1cc40f41d5aff6f91e0ead3a82258a839847e84d788462b153dde766b3`)
fails 14 of the 16 assignment/incdec and restored loop/visitor cases: four
assignment cases fail at runtime, eight incdec/loop cases fail compilation,
and the two via-IR visitor cases return the wrong trace. All 16 pass with the
fixed compiler. The two legacy visitor cases pass before and after.

These two fixes touch only `SolAssignment.cpp` and `SolUnaryOperation.cpp` in
`src/`: 28 lines added, 34 removed (**−6**). Combined with the preceding five
refactors, the `src/` diff is 280 added, 571 removed (**−291**). The memory
representation and dependency pins are unchanged.

Full verification used compiler SHA-256
`cc5fe0ef38dfe46c10d70382911d2605d6c4c1182f1182a8e4f96946f21d8ec6`,
fixed throughout testing, with `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/
-q -n 3 --tb=short --junitxml=/tmp/puya-sol-assignment-incdec-20260909.rkGuKc/semantic-final.xml`:
**1,946 passed, 1 failed, 101 xfailed, 38 xpassed** (2,086 total), in 253.15
seconds. All 2,078 preceding cases retained their JUnit outcome, and all eight
new cases passed. The lone failure is the unchanged
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(uint256)(0)` returns zero
instead of reverting. No xfail/XPASS markers changed; this remains a failure,
not an accepted divergence.

Focused tests passed 28/28, native CTests 19/19, AVM standard-library runtime
tests 20/20, and chainwide harness units 91/91. Evidence, the independent solc
oracle, and the full case-by-case comparison are retained locally in
`/tmp/puya-sol-assignment-incdec-20260909.rkGuKc/`. At this checkpoint, changes
were uncommitted on `rev-2`, based on `7bf4febee32fcca6a14f154016eb86c54166b321`.

A separate minimal probe with `returns (uint64[1] memory r, uint64, uint64)`
failed with `tuple item names are not unique` on both the saved pre-fix compiler
and the compiler used for that batch. It was not an assignment/incdec regression
or an accepted divergence. The value tests temporarily used consistently named
return parameters; the follow-up below restores their mixed return lists.
The original reproducer and both compiler outputs are retained in
`/tmp/puya-sol-assignment-incdec-20260909.rkGuKc/mixed-returns-{baseline,current}/`
with the source alongside them as `mixed_returns.sol`.

### Mixed return lists — 2026-09-09

The return plan now emits optional AWST tuple field names only when every
return declaration has a name. Solc return values are positional, and a mixed
list must not turn its empty names into duplicate fields. Fully named tuple
metadata is preserved; named Solidity locals still use their original names.
The shared implicit-return builder also emits the declared type's default
value for each unnamed result, rather than referencing an empty variable name.
This follows solc's initialization of every return declaration, named or not.

[Mixed-return regressions](tests/puyasolRegression/test_mixed_returns.py) cover
names at the beginning, middle and end; one or multiple unnamed results;
explicit returns, branch fallthrough, scalar and aggregate defaults; internal
forwarding, free/library calls with memory write-back, modifier bodies
(extended to repeated placeholders by the follow-up below) and skipped bodies;
fully named and fully unnamed controls. They run in
all eight combinations of ARC-4/EVM ABI, named/slot storage, and legacy/via-IR
behavior. Structural assertions reject empty or duplicate AWST tuple field
names and verify that fully named metadata remains present.

Independent solc 0.8.34/PyEVM checks pass **66/66** (33 per mode, optimizer 200
runs, Cancun), including the original reproducer. The previous compiler fails
all eight mixed-return configurations during compilation; the fixed compiler
passes them. The incdec fixture's original mixed return declarations are also
restored. The implementation adds **five net lines** across `ReturnWirePlan.cpp`
and `FunctionBuilder.cpp`, without a new abstraction or memory-model change.

Full verification used compiler SHA-256
`98447acbbd2de48860c87fa571777b15e1ee79192be64c3595c15b96314f2e3a`,
fixed throughout testing, with `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/
-q -n 3 --tb=short --junitxml=/tmp/puya-sol-mixed-returns-20260909.wdMW5W/semantic-final.xml`:
**1,954 passed, 1 failed, 101 xfailed, 38 xpassed** (2,094 total), in 221.13
seconds. All 2,086 preceding cases retained their JUnit outcome; the eight
new mixed-return cases passed. The only failure remains the existing Puya
DCE/divide-by-zero case `test_dce_reverting_subexpr_literal_folds`. No markers
or dependency pins changed, and LocalNet reset remained disabled.

Focused tests passed 36/36, native CTests 19/19, AVM standard-library runtime
tests 20/20, and chainwide harness units 91/91. Reports, the solc oracle,
saved pre-fix compiler, and case-by-case comparison are retained locally in
`/tmp/puya-sol-mixed-returns-20260909.wdMW5W/`. At this checkpoint, changes were
uncommitted on `rev-2`, based on `7bf4febee32fcca6a14f154016eb86c54166b321`.

### Modifier return inputs and outputs — 2026-09-09

An initial broader probe exposed a pre-existing via-IR mismatch for repeated
modifier placeholders. This also reproduces without any mixed returns:
`modifier twice() { _; _; }` with `f(uint64 x) ... twice returns (uint64 value)
{ value += x; }` returned 14 for `f(7)` in both puya-sol modes, including with
the saved pre-fix compiler. Solc returns 14 in legacy mode but 7 via-IR.

This is now fixed in `ModifierChainBuilder.cpp`. Following pinned solc's
`IRGenerator::generateModifier`, each via-IR modifier has separate return
inputs and outputs. Outputs are initialized from inputs **before** modifier
argument evaluation. Each `_` calls the next link with the source-local inputs
and captures the result into the separate outputs; fallthrough and bare
modifier returns use those outputs. Legacy still accumulates across repeated
placeholders. Actual memory/storage mutations and the write-back transport
remain shared; this does not change the memory model.

The broader regression also exposed a pre-existing type mismatch for narrow
signed return inputs: a body using native `int16` operations received the
method's ABI-normalized `biguint` carrier. Return locals now use the solc-derived
numeric representation, with the existing call-result decoder and return
element conversion adapting the emitted method signature at each boundary.
No new conversion framework is introduced. Both fixes add **31 net lines** in
`src/`, confined to the modifier-chain implementation.

[Modifier return regressions](tests/puyasolRegression/test_modifier_returns.py)
cover repeated, nested, looped, skipped and early-returning modifiers; explicit,
implicit, unnamed, scalar and tuple returns; return-variable assignments in
modifier arguments; negative narrow signed inputs/results; and memory/storage
side effects. They run all eight combinations of ARC-4/EVM ABI, named/slot
storage, and legacy/via-IR behavior. The mixed-return fixture's repeated
placeholder probe is restored too.

Independent solc 0.8.34/PyEVM checks pass **132/132** (63 regression checks per
mode plus six checks of existing upstream fixtures, optimizer 200 runs,
Cancun). Two old via-IR Python assertions incorrectly expected legacy
accumulation (10 and 2); both are corrected to 1, matching their **unchanged
Solidity fixtures** and independently executed solc results. No expectation
markers are added or weakened. The saved compiler also fails the narrow
signed fixture during compilation and returns 14 instead of 7 in the mixed
repeated-placeholder probe.

Full verification used compiler SHA-256
`a6016b788b9f5554f1c0157fadf479976e53602e5925e1c20606851b4a37ff80`,
fixed throughout testing, with `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/
-q -n 3 --tb=short --junitxml=/tmp/puya-sol-modifier-returns-20260909.iafs2A/semantic-final.xml`:
**1,962 passed, 1 failed, 101 xfailed, 38 xpassed** (2,102 total), in 317.81
seconds. All 2,094 preceding cases retained their JUnit outcome; the eight
new modifier-return configurations passed. The only failure remains the
existing Puya DCE/divide-by-zero case
`test_dce_reverting_subexpr_literal_folds`. No markers or dependency pins
changed, and LocalNet reset remained disabled.

Focused modifier/mixed-return tests passed 47/47, native CTests 19/19, AVM
standard-library runtime tests 20/20, and chainwide harness units 91/91.
A separately deployed minimal `f(7)` reproducer confirms 14/14 before the fix
and 14/7 afterward (legacy/via-IR). Reports, the solc oracle and emitted Yul,
saved pre-fix compiler, standalone before/after artifacts, and case-by-case
comparison are retained locally in
`/tmp/puya-sol-modifier-returns-20260909.iafs2A/`. At this checkpoint, changes
were uncommitted on `rev-2`, based on `7bf4febee32fcca6a14f154016eb86c54166b321`.

### Statement simplification (items 2–6) — 2026-09-09

Statement wrapper classes are unchanged. This batch removes the return paths
that solc's type checker rules out, shares effectful condition lowering, and
routes scalar/tuple declaration conversions and bindings through the same
helpers. The conversion inputs are solc's analyzed source/destination types;
slot-backed storage-to-memory copies use the existing materialization helper.
Assembly preparation shares external-reference classification and indexes
initializer provenance once by solc declaration ID, with live slot bindings
taking precedence. Local storage-reference `.offset` uses solc's zero-offset
fact, including in named-storage mode.

Initializer prerequisites now precede their binding. Assembly-backed
`new bytes(n)` / `new string(n)` chooses its representation before lowering
`n`, so memory-mutating length calls are evaluated once. Tuple memory aliases
resolve the original stable declaration, not a copied tuple temporary. No
memory-model redesign is included.

Relative to the pre-batch source snapshot, `src/` changes by **301 added,
543 removed: net −242 physical lines**, including comments and headers.
This is incremental to the earlier uncommitted block/visitor/modifier work,
not the combined diff against `HEAD`.

The regressions in [test_statement_bindings.py](tests/puyasolRegression/test_statement_bindings.py)
cover conversions, storage copies and references, memory aliases, single
initializer evaluation, void-return effects, all loop kinds/continue,
assembly declaration shadowing/member aliases/live slot reassignment, and
side-effecting calldata slice indices. They exercise legacy/via-IR,
named/slot storage, and ARC-4/EVM ABI. Independent solc 0.8.34 + PyEVM
execution confirms **80/80 calls** (40 per code-generation mode, optimizer
200 runs, Cancun).

The separately retained `test_tuple_storage_return_identity` exposes an
existing function-return protocol bug: in named-storage mode, an opaque
multi-return call copies the returned structs instead of retaining their
storage identity. On a fresh deployment, `tupleReferences()` returns
`(0, 0, 1)` instead of solc's `(23, 29, 1)`; the saved pre-batch compiler
does the same. Slot mode preserves the references. Fixing this requires a
per-element storage-handle return convention, not reconstructing a lost
reference in the declaration builder. These cases remain ordinary failing
assertions: no xfail markers or weakened expectations hide the gap.

Full verification used compiler SHA-256
`41ce016d5b6077e13d97602083059a4fdcc427e8ca6a889ebd240d98783c7889`.
All 40 new source/profile combinations compile successfully; native CTests
pass 19/19, AVM standard-library runtime tests 20/20, and chainwide harness
units 91/91. The final complete semantic run, from this directory, used:

```bash
PUYASOL_LOCALNET_RESET=0 \
PUYA_SOL_CACHE_DIR=/tmp/puya-sol-statements-20260909.yCeWKA/restored-localnet-cache \
pytest tests/ framework/ -q -n 3 --tb=short \
  --junitxml=/tmp/puya-sol-statements-20260909.yCeWKA/semantic-final.xml
```

Result: **2,006 passed, 5 failed, 100 xfailed, 39 xpassed** (2,150 total),
in 565.76 seconds. Case-by-case comparison with all 2,102 preceding tests
found **no regressions**: 2,101 outcomes are unchanged, and the previously
xfailed `abiEncoderV1::test_dynamic_memory_copy` now passes. No expectation
markers were changed. Of the 48 new test configurations, **44 pass and
four fail**, all four being the named-storage reference-return case above.
Compared with the saved pre-batch compiler, 32 new configurations improve
from failure to pass. The fifth full-suite failure is the already-known
Puya DCE/divide-by-zero case `test_dce_reverting_subexpr_literal_folds`.
The suite is therefore **not entirely green**, despite no existing-test
regressions.

An earlier run was interrupted around 69% when LocalNet became unavailable.
After LocalNet was restored, its existing KMD service was resumed without
resetting the ledger. The first completed rerun encountered seven additional
artifact-read failures: existing cached JSON/TEAL included zero-filled files.
All seven passed with a fresh cache and the unchanged compiler. The entire
suite was then rerun with that fresh cache to produce the final result above;
the earlier cache and failed-run reports are retained for diagnosis.

The source snapshot, saved pre-batch compiler, solc oracle/results,
compile-only matrix, focused before/after reports, interrupted/cache-failure
logs, final full-suite JUnit report, and case-by-case comparison are retained in
`/tmp/puya-sol-statements-20260909.yCeWKA/`. At this checkpoint, changes were
uncommitted on `rev-2`; `memory_redesign.md` remained untouched and untracked.

### Expression audit (all seven items) — 2026-09-09

The expression batch uses solc source/destination types for conditional,
tuple, inline-array, transient and blob-store conversions. Exponentiation
retains the exponent's own solc mobile type. Checked index narrowing and
slice bounds are shared, including discarded slices and typed `[:]` slices.
Assignment expressions return the assigned value or destination reference.

The concrete `ResolvedLValue` class shares destination evaluation, native
reads, stores and deletion between ordinary assignments, compound updates,
increment/decrement and tuple leaves. Dedicated aggregate-copy/page-lifecycle
policies remain. Ternary and short-circuit expressions share conditional
effect emission; integer/bool unary operations use the existing typed
builders, with only the fixed-bytes complement fallback retained. This does
not redesign memory or replace the concrete expression wrapper classes.

Full-suite feedback corrected dropped box-declaration metadata, absent-box
default reads, addressed aggregate/tuple writes, nested tuple type plumbing,
string store adaptation, sparse-array deletion, and an old return-normalizer
that rewrote native named-return stores to their promoted return carrier.
The two obsolete assignment implementation files are removed; their tracked
versions remain recoverable in Git.

Relative to the pre-expression snapshot, `src/` changes by **806 added,
2,480 removed: net −1,674 physical lines**, including the new class and
headers. This excludes the earlier uncommitted statement/modifier changes.

The five fixtures in
[test_expression_lowering.py](tests/puyasolRegression/test_expression_lowering.py)
exercise legacy/via-IR, named/slot storage and ARC-4/EVM ABI: 40 configurations.
The independent solc 0.8.34 + PyEVM oracle passes **288 checks** (144 per
code-generation mode, Cancun, optimizer 200), including signed widening,
wide exponents/indices, nested tuple holes, assignment-result references,
single evaluation, conditional effects and unary boundaries.

Full verification completed on **2026-09-10**, using compiler
SHA-256 `573e27fb1837e03334b43fee3ae479669b6637a5fdee19a65a3f8954b163adf9`.
The final full semantic result is **2,046 passed, 5 failed, 100 xfailed,
39 xpassed** (2,190 total), in 1,114.83 seconds. All **40 new expression
configurations pass**, including the final named-storage alias assignment
result and expanded reference/nested-tuple cases. Case-by-case JUnit comparison
with the pre-expression baseline confirms **all 2,150 existing outcomes are
unchanged**, with no removed cases. The compiler hash stayed unchanged
throughout the run; no expectation markers were changed.

The full suite at this checkpoint was **not entirely green**: five pre-existing
failures remained open. One is the pinned-Puya DCE divide-by-zero case
`test_dce_reverting_subexpr_literal_folds`; the other four are
`test_tuple_storage_return_identity` in named-storage mode (legacy/via-IR,
ARC-4/EVM ABI), where opaque multi-return calls lose storage-reference identity.
None is a new expression-batch regression.

Supporting verification passes: native CTests **19/19**, AVM standard-library
runtime tests **20/20**, chainwide harness units **91/91**, semantic harness
units **8/8**, and the compile-only expression profile matrix **40/40**.
The compile-only matrix used the standalone backend (`PUYA_SOL_NO_SERVE=1`)
after the sandboxed persistent-server run stalled; its successful report is
`compile-matrix-resumed.log`.

An intermediate full run exposed 28 new compilation/assertion regressions
and a test-module name collision. The corrections passed an 80-case focused
runtime rerun before the final full run above. Later runtime attempts were
blocked in setup when LocalNet/Docker became unavailable. The user restored
LocalNet with a ledger reset; final verification deployed fresh contracts on
that ledger, with automatic resets disabled. No reset was performed by the
agent. All intermediate reports are retained rather than treated as current
validation results.

The final full run, from this directory, used:

```bash
PUYASOL_LOCALNET_RESET=0 \
PUYA_SOL_CACHE_DIR=/tmp/puya-sol-exprs-20260909.y8zFC8/final-runtime-cache \
pytest tests/ framework/ -q -n 3 --tb=short \
  --junitxml=/tmp/puya-sol-exprs-20260909.y8zFC8/semantic-final.xml
```

The AVM-library rerun used its separate `stdlib-restored-cache` and passed
in 17.97 seconds. The snapshot, saved baseline compiler, oracle, build/compile
reports, intermediate runtime logs, `semantic-final.xml`,
`semantic-comparison.json` and `stdlib-restored.xml` are retained in
`/tmp/puya-sol-exprs-20260909.y8zFC8/`.
At this checkpoint, changes were uncommitted on `rev-2`;
`memory_redesign.md` remained untouched and untracked.

### Storage-reference identity and sol-types refactors — 2026-09-10

Mixed internal returns now transport storage references as logical slots,
using solc declaration identities, reference locations and canonical storage
layout. The transfer graph also covers locals rebound by a later tuple
assignment. Forwarded calls and conditional tuples preserve reference identity
and evaluate only the selected branch. The named-storage dispatcher bridges
live scalar-array element words to the existing ARC4 array, using solc packing
facts and the shared slot codec; no second storage copy is introduced.

The seven sol-types changes are:

1. Shared ARC4 size/default traversal, including packed booleans, checked
   capacities and explicit rejection of unsupported defaults.
2. Solc-typed conversions for struct constructor arguments, array pushes,
   constant initializers, new-contract arguments and using-for receivers.
   Function compatibility and nominal UDVT checks no longer bypass solc.
3. One array-conversion implementation selecting packed conversion, fixed
   copying or an integer-widening loop before emitting effects.
4. Separate caches for full recursive structs and finite projections, with
   retained solc provenance and explicit adaptation between representations.
5. ABI names derived from emitted parameter/return wire plans instead of a
   parallel Solidity-type naming implementation.
6. One tuple adapter that snapshots values before effectful conversions and
   does not mutate shared source AST nodes. Implicit returns retain their
   actual native component types until wire encoding. Solc's UTF-8 validator
   keeps arbitrary byte literals out of JSON string constants before snapshots.
7. Removal of redundant coercion wrappers, accessors, naming policies and
   manual sign-extension temporary plumbing.

The storage/sol-types batch changes `src/` by **901 added, 1,488 removed
(net −587)** against the saved expression-batch snapshot. Including the earlier
pending statement/expression/modifier work, the full `src/` change is **2,338
added, 5,096 removed (net −2,758)** against `7bf4febee32fcca6a14f154016eb86c54166b321`.
These totals include the new source headers/files. The memory model and
dependency pins are unchanged; `memory_redesign.md` stays untracked.

Recursive projections now have a deterministic shape independent of type-cache
population order. Validation uses fresh deployments; compatibility with
previously deployed recursive aggregate encodings is not established by these
tests. The existing named-storage dynamic-`bool[]` rejection is unchanged.

The independent solc 0.8.34 / PyEVM oracles pass **108 checks** (54 per legacy
and via-IR mode), covering the new conversions and reference operations.
Focused constant/array-reference runtime checks pass **21/21**; the subsequent
byte-literal/sol-types/reference rerun passes **27/27**. Evidence is
retained locally in `/tmp/puya-sol-soltypes-20260910.iLLyOm/`.

Final full verification used compiler SHA-256
`7b20e8ff7a2c55ba1b97b7d51c5bf485a4fdab330353538fa33c08fd04c23dcc`,
unchanged throughout testing. Result: **2,086 passed, 1 failed, 100 xfailed,
39 xpassed** (2,226 total), in 333.03 seconds. Case-by-case comparison with the
saved 2,190-case expression baseline confirms that the four named-storage
identity failures now pass, the other **2,186 outcomes are unchanged**, and
all **36 added cases pass**. No cases were removed and no expectation markers
changed. Native CTests pass **21/21**, AVM standard-library runtime tests
**20/20**, and chainwide harness unit tests **91/91**.

The suite is **not entirely green**: the unchanged pinned-Puya failure
`test_dce_reverting_subexpr_literal_folds` remains an ordinary failure.
`divdivShl(uint256)(0)` still returns zero instead of reverting. This is not an
accepted divergence and was not hidden with an xfail marker.

The full run used `PUYASOL_LOCALNET_RESET=0`, the `verified-runtime-cache` in
the evidence directory, and `pytest tests/ framework/ -q -n 3 --tb=short`.
A local reporting-only `live_failures` hook printed failures as they arrived.
The current reports are `semantic-verified-2.xml`, `semantic-verified-2.log`,
`semantic-comparison.json`, `ctest-final.log`, `stdlib-verified.xml`, and
`chainwide-verified.xml`. Earlier failed runs remain in the same directory for
diagnosis; they are not the final validation result. The last corrections
preserve native implicit-return carriers, literal padding behind tuple
snapshots, typed constant chains, and non-UTF-8 byte literals. No LocalNet
reset, dependency change, memory-model redesign, CI change, or XPASS review
was performed for this batch.

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
