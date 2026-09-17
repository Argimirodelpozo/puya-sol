# Solidity semantic tests

The suite combines ported upstream Solidity semantic fixtures with puya-sol
regressions. Explicit Python assertions in `tests/<category>/test_*.py` are the
test oracle; Solidity `// ----` comments are not parsed at runtime. The old
parser and analysis scripts remain under `legacy/` for historical reference,
not as an active test runner.

## Recorded baseline

### Lowering reductions and Solidity/Yul boundaries — 2026-09-17

This commit on `rev-2` follows `c65593221f`. Empty literal and runtime calls
share application/account transport and replace returndata on every successful
path. External-call encoding, ASA operand sequencing and function-pointer
construction are shared; the RIPEMD helper cleanup preserves its complete AWST
subroutine. Assembly-visible scalar locals retain full words across blocks and
same-type copies. Calldata uses immutable caller frames, dominating parameter
initialization and declaration-keyed, solc-shaped references for aliases,
slices, rebinding and typed reads. Internal direct, virtual and function-pointer
calls preserve the caller frame. The memory experiment remains separate.

| Result | Count |
|---|---:|
| Passed | 2,805 |
| Failed | 0 |
| Expected failure (xfail) | 99 |
| Unexpected pass (xpass) | 40 |
| Total | 2,944 |

The final full LocalNet semantic and framework repeat took **999.55 seconds**:
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 2 --tb=short
--junitxml=out/lowering-boundaries/semantic.xml`. All **2,887 existing individual
outcomes are unchanged**; all **57 new configurations pass**, comprising 49
runtime configurations and eight explicit compiler-rejection checks. No cases
were removed, failure markers changed, ledger reset or compile cache cleared.

Native CTests passed **24/24** in 3.00 seconds. The final focused repeat passed
**217/217** in 165.69 seconds, including four super-resolution regressions found
and fixed before this full repeat. Official solc **0.8.34+80d5c536** execution
records **146 passing probes** across optimized legacy/via-IR; this oracle is
distinct from the frontend's clean pinned solc source checkout. The complete
`__builtin_ripemd160` AWST subroutine is unchanged, including source locations.
Compiler, production-source and test manifests matched after validation; the
compiler SHA-256 is
`a5425b3dc191bc78cd36ef77c15355ac74d719f21d70ec7370e0d3168612b685`.

Returning a calldata reference through an internal function and binding it in a
calldata-observing body still rejects explicitly; arbitrary dirty scalar-word
transport across function/ABI boundaries is not promised. Existing external
self-call and staticcall divergences are unchanged. See
[known limitations](../../docs/KNOWN_ISSUES.md) and
[call transport adaptations](../../EVM_DIVERGENCE.md).

`lowering/` removes **193 physical / 133 code lines**. The complete correctness
and reduction pass adds **433 physical / 469 code lines** to `src/`, leaving
**62,481 physical / 48,460 code lines** in 320 files. The
[bytecode-size comparison](../sizes/reports/lowering-boundaries.md) separates
existing-program deltas from new fixtures. The
[JUnit report](out/lowering-boundaries/semantic.xml),
[outcome comparison](out/lowering-boundaries/semantic-comparison.json),
[console output](results.txt), oracle evidence and hash manifests are retained
as thin reports; raw generated artifacts remain local and ignored.

### Storage/codec/memory correctness — 2026-09-16

This commit on `rev-2` follows the reduction checkpoint `e94bbc6218`. Memory
defaults now use solc's memory sizes, strides, member offsets and zero pointer;
nested reference writes, tuple swaps and deletion preserve aliases. Storage
lengths are checked before narrowing, packed-address metadata follows changed
word lanes, and bounded array materialization/deletion uses the existing loops
beyond the obsolete 64-element limit. Enums retain complete words until range
validation; narrow Yul locals retain raw words within an assembly block.
Explicit division/modulo guards fix the known DCE regression without changing
the pinned Puya dependency. The separate memory experiment is unchanged.

| Result | Count |
|---|---:|
| Passed | 2,748 |
| Failed | 0 |
| Expected failure (xfail) | 99 |
| Unexpected pass (xpass) | 40 |
| Total | 2,887 |

The full LocalNet semantic and framework repeat took **1,162.94 seconds** with
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 2 --tb=short
--junitxml=out/storage-codec-correctness/semantic.xml`. The DCE case changes
from failure to pass; `viaYul/test_viaYul.py::test_dirty_memory_struct` changes
from xfail to xpass. All other existing outcomes are unchanged. Twelve new
configurations pass, and one obsolete capacity-rejection test is explicitly
renamed to cover successful compilation and bounded runtime deletion. The two
slot-mode fixed-array conversion configurations also exercise runtime results
instead of expecting the removed cap. No failure markers were changed.

Native CTests passed **24/24** in 5.66 seconds; the expanded focused repeat
passed **60/60** in 118.25 seconds; pinned-solc legacy/via-IR execution confirmed
**260/260** oracle expectations. Two forged maximal-length probes differ between
solc backends: legacy wraps the length, while via-IR rejects it. AVM explicitly
rejects those lengths before materialization; the oracle records this boundary.
The old lazy-box fixture's 644-slot deletion exceeds AVM box-reference resources,
so it has compile-success coverage plus a packed 129-element runtime deletion
and isolation check. The existing 258-element slot conversions now run as well.

An earlier interrupted run required restarting the existing KMD service; the
ledger and compile cache were preserved. The first completed repeat found a
bool-getter carrier mismatch and three obsolete cap expectations, all corrected
before this final full repeat. Compiler, source and test manifests match the
validated build, whose SHA-256 is
`d868e7e38ff4629fa5832db51c31267887a679d07172e23f33a0f0e074f7e7ed`.

Standalone named enum cells and enum mapping-key payloads change representation
and require fresh deployments; ABI and packed-field widths are unchanged.
See [the storage-format compatibility notes](../../docs/storage-format.md).
This correctness pass adds **211 physical / 203 code lines** to `src/`, leaving
**62,048 physical / 47,991 code lines** in 318 files. Combined with the preceding
reduction pass, the net change is **495 fewer physical / 360 fewer code lines**.
The [JUnit report](out/storage-codec-correctness/semantic.xml),
[outcome comparison](out/storage-codec-correctness/semantic-comparison.json),
[console output](results.txt), oracle evidence and hash manifests are retained
as thin reports; raw generated outputs remain ignored.

### Storage/codec/Yul reductions — 2026-09-16

This commit on `rev-2` follows the validated parenthesis checkpoint
`dd538fd32e`. It removes unused slot helpers and dead Yul bookkeeping, shares
signed cleanup and byte-carrier facts, loops larger fixed-array memory reads,
and shares memory-range and multi-return lowering. Yul switches compare complete
words; hashes read actual memory instead of guessing from calldata coordinates;
overlapping writes invalidate content facts. Unproven free-memory-pointer
alignment is no longer assumed. The memory experiment remains separate.

| Result | Count |
|---|---:|
| Passed | 2,735 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,875 |

The full LocalNet semantic and framework repeat took **978.83 seconds** with
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 2 --tb=short
--junitxml=out/storage-codec-yul/semantic.xml`. All **2,859 previous cases retain
their individual outcomes** and all **16 new configurations pass**; no cases
were removed or failure markers changed. The single failure remains the known
Puya DCE/divide-by-zero case, not an accepted divergence. No ledger or compile
cache reset was performed.

Native CTests passed **24/24** in 2.85 seconds; focused regressions passed
**16/16** in 58.47 seconds; independent pinned-solc legacy/via-IR execution
confirmed **84/84** checks. Coverage includes packed signed storage, fixed and
nested memory arrays, full 4096-byte values, switches, overlapping stores,
hash/revert ranges, poisoned pointer alignment and multi-return side effects.
The final compiler SHA-256 was
`1bc48dbd4247f180dae6f5bd4fcac4330cc7a4e26d76c4a9829a3c0f29b71625`.
Compiler, production-source and regression manifests matched after the run.

The pass removes **706 physical lines / 563 code lines** from `src/`, leaving
**61,837 physical / 47,788 code lines** in 318 files. The cumulative source
reduction since `18804ec53f`, including the preceding audits, is **3,036 physical
lines**. The [JUnit report](out/storage-codec-yul/semantic.xml),
[outcome comparison](out/storage-codec-yul/semantic-comparison.json),
[console output](results.txt), oracle evidence and hash manifests are retained;
raw generated outputs remain ignored.

The first fixture drafts also exposed existing default nested-memory allocation
and nested-byte-reference assignment limitations, recorded in the local,
untracked storage/codec/Yul audit. Explicit initializers isolate reader coverage;
those limitations and the remaining storage/enum audit bugs are not new xfails
or claimed fixed by this reduction pass.

### Parenthesized-expression boundaries — 2026-09-16

This checkpoint on `rev-2` includes the preceding AST, lowering, core and
typed-builder/contract audits. Structural expression queries now normalize
solc's singleton grouping tuples through `SolcFacts::expressionAs`, retaining
real tuples, holes, inline arrays, declaration identity and call options.
A source guard catches new raw expression-shape pointer casts.

| Result | Count |
|---|---:|
| Passed | 2,719 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,859 |

The full LocalNet run took **743.24 seconds**, with
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 2 --tb=short
--junitxml=out/parenthesis-regressions/semantic.xml`. All **2,826 previous cases
retain their individual outcomes**, and all **33 new configurations pass**.
The only failure remains the known Puya DCE/divide-by-zero regression, not an
accepted divergence. No ledger or cache reset was performed.

Native CTests passed **24/24** in 2.56 seconds. Focused validation passed
**41/41** in 14.88 seconds on a serial repeat; the first parallel run had one
duplicate budget-helper transaction infrastructure failure. The pinned-solc
runtime oracle passed **60/60** checks across legacy/via-IR and grouping depths.
The separate grouped-custom-error test covers an upstream solc code-generation
`std::bad_cast`; its ungrouped form supplies the EVM reference.

The compiler SHA-256 was
`99c03264c88f7e513e3fda79cbf37064253df352495ce614de6ae4029d332c41`.
Production-source and regression hash manifests matched after the full run.
This correction removes **11 physical lines / 13 code lines** from `src/`,
leaving **62,543 physical / 48,351 code lines** in 318 files. Including the
preceding audits, the source delta is **2,330 fewer physical lines**.
The [JUnit report](out/parenthesis-regressions/semantic.xml),
[outcome comparison](out/parenthesis-regressions/semantic-comparison.json),
oracle evidence and hash manifests are retained as thin reports; raw generated
outputs remain ignored. The memory experiment remains separate and unchanged.

### Typed-builder/contract-emission audit — working-tree verification, 2026-09-16

All 16 audit items are implemented as uncommitted changes on `rev-2` atop
`18804ec53f`, including the preceding AST, lowering and core work. Arithmetic
cleanup uses solc widths; explicit aggregate initializers follow solc's
constructor schedule; modifier arguments share typed conversion and storage
reference resolution. Unknown-slot reads do not allocate boxes, bytes-storage
headers are validated, and ABI entry checks are separate from internal bodies.
Unused builder APIs are removed, builtin dispatch uses solc function kinds,
byte conversions and getter finishing are shared, and EVM routes retain stable
method indices. The memory experiment remains separate and unchanged.

| Result | Count |
|---|---:|
| Passed | 2,686 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,826 |

The completed LocalNet semantic and harness/cache repeat took **783.27 seconds**
with `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 2 --tb=short
--junitxml=out/eb-contract-audit/semantic.xml`. All **2,782 previous cases retain
their individual JUnit outcomes**, and all **44 new configurations pass**.
No cases were removed and failure markers are unchanged. The only failure
remains the existing Puya DCE/divide-by-zero case described below, not an
accepted divergence. An earlier repeat was interrupted by WSL stopping; the
completed run used two workers after recovery without a ledger or cache reset.

Native CTests passed **24/24** in 4.68 seconds. The expanded focused matrix had
**186 passed and one existing xfail** in 77.33 seconds. An independent
pinned-solc/EVM probe confirmed **100 scenarios** in legacy and via-IR modes.
The compiler stayed fixed at SHA-256
`18f1429aa3bd77403d4b004d5271c3c4cacaa810dab33c95b50f0b6441f09627`;
all production-source and regression hashes matched after the run. Dependency
pins are unchanged.

This pass removes **1,216 physical lines from `src/`**, including **891 code
lines** excluding comments and blanks. The source tree now has **48,364 code
lines**; the cumulative source reduction including AST, lowering and core work
is **2,319 physical lines**. The [JUnit report](out/eb-contract-audit/semantic.xml),
[outcome comparison](out/eb-contract-audit/semantic-comparison.json), and
[console output](results.txt) record the final run. Thin reports, oracle evidence
and hash manifests stay under `out/eb-contract-audit/`; raw artifacts stay ignored.

### AWST/runner/CLI/type-boundary audit — working-tree verification, 2026-09-15

All nine audit items are implemented as uncommitted changes on `rev-2` atop
`18804ec53f`, including the preceding AST and lowering work. Constructor scalar
decoding uses solc ABI-coder facts; shared AWST graphs use pinned-Puya JSON
references; tuple immutability matches the backend; unreachable ReferenceArray
paths and duplicate scalar conversions are removed. CLI cancellation, legacy
import identity, required ARC56 artifacts and option handling are covered by
regressions. The memory experiment remains separate and unchanged.

| Result | Count |
|---|---:|
| Passed | 2,642 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,782 |

The full LocalNet semantic and harness/cache run took **1,294.89 seconds** with
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3 --tb=short
--junitxml=out/core-audit/semantic.xml`. All **2,759 previous cases retain their
individual JUnit outcomes**, and all **23 new configurations pass**. No case was
removed and failure markers are unchanged. The only failure remains the existing
Puya DCE/divide-by-zero case described below, not an accepted divergence.

Native CTests passed **24/24** in 2.30 seconds; focused regressions passed
**48/48** in 18.21 seconds. The independent pinned-solc/EVM probe records
**56 constructor scenarios** across ABI coders v1/v2 and legacy/via-IR modes.
The compiler remained fixed at SHA-256
`141b2d86e9659c7b3a1f9bbe088f7f74f6b48aab31042c1e71fb7239428d9aab`;
production-source and regression hashes matched before and after the run.
Dependency pins are unchanged. Neither LocalNet nor the compile cache was reset.

The local, untracked core audit records the nine changes and net
**179-line reduction in `src/`**, including **132 fewer code lines** excluding
comments and blanks. The final tree has **49,255 code lines**; the cumulative
source delta including AST and lowering is **1,103 fewer physical lines**.
The [JUnit report](out/core-audit/semantic.xml),
[outcome comparison](out/core-audit/semantic-comparison.json), and
[console output](out/core-audit/semantic.txt) record this run. Thin reports, oracle evidence and
hash manifests remain under `out/core-audit/`; raw generated artifacts remain
local and ignored.

### Lowering/parenthesis audit — working-tree verification, 2026-09-15

The ten-item lowering audit and the parenthesized-expression follow-up are
implemented as uncommitted changes on `rev-2` atop `18804ec53f`, including the
preceding AST audit. Call routing, external overrides, constant precompile
addresses and intrinsic recognition use solc facts. Outgoing encoders and
transaction submission share their existing implementations; self-call routing,
returndata and function-pointer option/static-context handling are corrected.
The mixed memory representation is unchanged, and the experiment stays separate.

| Result | Count |
|---|---:|
| Passed | 2,619 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,759 |

The final full LocalNet semantic and harness/cache repeat took **1,045.08
seconds**, using `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3
--tb=short --junitxml=out/lowering-audit/semantic.xml`. All **2,703 previous
cases retain their individual outcomes**; all **56 new configurations pass**,
with no removals or changed failure markers. The only failure remains the
existing Puya DCE/divide-by-zero case described below.

Native CTests passed **24/24** in 3.61 seconds. Focused runtime validation
passed **58/58** in 59.97 seconds, including the two existing cases corrected
after the first full run. The independent pinned-solc/PyEVM oracle passed
**74 assertions**, covering legacy and via-IR compilation. The compiler stayed
fixed at SHA-256
`b69df362bcaea1e768f6c62a1833aecd2051d2b10d3a16313508f8776fbb4809`;
production-source and regression hashes also matched before and after the run.
Dependency pins are unchanged. Neither LocalNet nor the compile cache was reset.

The local, untracked lowering audit records the ten changes,
remaining self-call limitations, and net **433-line reduction in `src/`** for
this pass (**924 fewer lines** including the AST work; physical lines).
The [JUnit report](out/lowering-audit/semantic.xml),
[outcome comparison](out/lowering-audit/semantic-comparison.json), and
[console output](out/lowering-audit/semantic.txt) record the final run. Thin reports, the first-run
evidence, the oracle script and hash manifests remain under `out/lowering-audit/`;
raw generated artifacts remain local and ignored.

### AST audit — working-tree verification, 2026-09-15

The nine-item AST audit is implemented on `rev-2` as uncommitted changes atop
`18804ec53f`. It uses solc facts for parenthesis/inline-array distinctions,
call options, self identity and external overrides; fixes storage/calldata
reference bounds and aggregate tuple copies; and consolidates assignment,
array-method and inline-assembly binding lowering. The existing mixed memory
representation is unchanged; the memory experiment remains separate.

| Result | Count |
|---|---:|
| Passed | 2,563 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,703 |

The full LocalNet semantic and harness/cache run took **981.75 seconds** with
three workers, using `PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3
--tb=short --junitxml=out/ast-audit/semantic.xml`. All 2,657 cases from the
preceding context-audit baseline retain their individual JUnit outcomes; all
46 added cases pass, with no removals. Expected-failure markers are unchanged.
The sole failure remains the Puya DCE/divide-by-zero bug described below.

Native CTests passed **24/24** in 4.51 seconds. Focused regressions passed
**48/48** in 25.87 seconds, including the two existing cases that exposed
regressions during the first full run. Independent solc/PyEVM checks passed
in legacy and via-IR modes. The final compiler stayed fixed at SHA-256
`ef1ef238e8dd6e6dbe5819db3a722889f6b2e04765bba3f165b7f9ad4a1f5fd4`.
Neither LocalNet nor the compile cache was reset, and dependency pins are
unchanged from the context-audit checkpoint below.

The local, untracked AST audit records all nine changes,
the net 491-line reduction in `src/`, and the separate pre-existing calldata
alias-metadata gap. The [JUnit report](out/ast-audit/semantic.xml),
[outcome comparison](out/ast-audit/semantic-comparison.json), and
[console output](out/ast-audit/semantic.txt) record the completed repeat. Thin reports and
hash manifests are retained under `out/ast-audit/`; raw generated outputs
remain local and ignored.

### Context-audit checkpoint

Full LocalNet semantic and harness/cache run on **2026-09-15**. The tested
compiler and regression sources are committed as `11803b9778` on `rev-2`,
following merge checkpoint `8a44a9ddda`. This context-audit batch derives
function/signature facts from solc, consolidates scoped translation state,
and fixes inherited storage offsets and tuple memory-reference identity.
The experimental memory model is not included:

| Result | Count |
|---|---:|
| Passed | 2,517 |
| Failed | 1 |
| Expected failure (xfail) | 100 |
| Unexpected pass (xpass) | 39 |
| Total | 2,657 |

The run took 391.74 seconds with three workers and a warm shared
compilation cache. It used
`PUYASOL_LOCALNET_RESET=0 pytest tests/ framework/ -q -n 3 --tb=short`
with JUnit reporting enabled.
Dependencies were pinned to Solidity
`a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d` and Puya
`27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1). Native CTest coverage
passed 24/24 in 4.45 seconds. Harness/cache unit tests are included in the full
count. Compared with the preceding layout checkpoint's full-run XML, all 2,647
existing cases retained their outcomes and no case was removed. All ten new
cases passed (eight LocalNet configurations and two frontend checks).

All 1,763 Solidity source files were compiled frontend-only in both storage
modes: 3,526 cases, with 3,271 successful compilations and 255 recorded frontend
failures. All 3,524 preceding cases retained their exit codes; the two added
cases succeeded. There are 55 changed AWST hashes: 53 from the previously
committed bytes-offset name fix (`5573dec612`) and two from tuple alias
preservation; no options hashes changed. The
[context-audit report](out/context-audit/REPORT.md) retains the full manifests,
focused runtime checks, 22 solc EVM expectations, and the cache-recovery record.
The [JUnit report](out/context-audit/semantic.xml),
[outcome comparison](out/context-audit/semantic-comparison.json), and
[console output](out/ast-audit/semantic-baseline.txt) retain the complete semantic result. The earlier
[directory-reorganization report](out/builder-layered-layout/REPORT.md) retains
its separate byte-identical move verification and residual dependency edges.

The compiler stayed fixed during the run at SHA-256
`f53ceaf6355a0a924f5750d78345c964fb69c5ed155bd16c7e10b20ba4c0e1de`,
and LocalNet was not reset. Its stopped KMD service was restarted without
touching the ledger. After an environment interruption, 72 damaged compile-cache
entries were recoverably quarantined; all 45 corruption-related failures passed
on retry before this final full run. See the
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

### Expression/member refactor verification (2026-09-10)

The `ast/exprs` and `ast/members` audit items 1–7 are implemented:

1. Paged-array, boxed-aggregate and offset-reference stores share
   `ResolvedLValue`. RHS evaluation precedes address effects; snapshots happen
   afterwards, and each tuple store observes earlier component writes.
2. Selectors use solc declarations or runtime external-function values, with
   receivers, conditional branches and call options evaluated exactly once.
3. Array lengths recognize logical storage handles in either layout; fixed
   lengths preserve receiver effects. Slice indexing/length share checked,
   full-width bounds and offset calculation.
4. Bare and qualified constants share declared-type `ConversionPlan` lowering.
5. Live calldata struct scalar fields use solc member offsets and the shared
   validated EVM-word decoder. Unsupported runtime members are errors, while
   solc metadata-only type/function expressions remain inert.
6. Signed arithmetic lives in the integer builder, with common/mobile operand
   types supplied by solc. The existing signed-multiply backend workaround is
   retained, and both integer and bytesN shifts use the RHS's own mobile type.
7. Field/element reads share typed ARC4 decoding, including signed carriers
   and writable bytes views. Enum ordinals and internal-call resolution use
   solc facts instead of local reconstruction.

The calldata field tests also required the existing synthetic encoder to unwrap
UDVTs before choosing word padding. This is a narrow signed/bytesN encoding
correction, not the deferred canonical-calldata redesign (item 8). Address
metadata policy (item 9) and the memory model were unchanged at this checkpoint;
item 9 is verified separately below.

The added matrix covers legacy/via-IR behavior, named/slot storage and ARC4/EVM
ABI profiles: **32 cases, 296 runtime checks**. The same assertions pass **74
independent solc 0.8.34/PyEVM checks** (37 per codegen). The final repair-focused
run reports **63 passed, 2 xpassed, 1 known failure**.

Final full verification: **2,118 passed, 1 failed, 100 xfailed, 39 xpassed**
(2,258 total), in 648.64 seconds. Case-by-case comparison confirms all **2,226
pre-existing outcomes are unchanged**, all **32 added cases pass**, and no
cases were removed. The sole failure remains
`test_dce_reverting_subexpr_literal_folds`: the pinned Puya backend returns zero
for `divdivShl(uint256)(0)` instead of reverting. It is still an ordinary
failure, not an accepted divergence or an xfail. Native CTests pass **21/21**,
standard-library runtime tests **20/20**, and chainwide harness unit tests
**91/91**.

Compiler SHA-256, unchanged throughout final validation:
`3119e56eeace9d845e29f56a54f09da942ac19eca4be371b9627d7b9f68420d5`.
Against `513468b7e1a75b112cb40ba965855406423bc9c3`, `src/` has **582 added,
1,707 removed lines (net −1,125)**. Excluding comments/blanks with cloc 1.90,
the reduction is **819 code lines**; `src/builder` now contains **47,975**.

Evidence is retained locally in `/tmp/puya-sol-exprs-members-20260910.UIp7Rk/`:
`semantic-final.xml`, `semantic-final.log`, `semantic-comparison.json`,
`focused-repair-2.xml`, `oracle.log`, `ctest-verified-2.log`,
`stdlib-verified.xml`, and `chainwide-verified.xml`. Earlier failed runs remain
for diagnosis and are not the final result. The full run used
`PUYASOL_LOCALNET_RESET=0`, the `semantic-cache` in that directory, and
`pytest tests/ framework/ -q -n 3 --tb=short` with a reporting-only live-failure
hook. No ledger reset, marker changes, dependency changes, CI changes, or XPASS
review were performed. `memory_redesign.md` remains untracked.

### Address metadata verification (2026-09-10)

Expression/member audit item 9 now shares `.code`, `.code.length` and
`.codehash` receiver lowering. Only self is hidden during construction;
deployed foreign applications remain queryable and receiver effects run once.
Zero cannot alias the current AVM application, and noncanonical address prefixes
do not resolve by matching low application-id bits. Solc's receiver type also
distinguishes an ordinary struct field named `code` from address metadata.

Approval-program bytes/hashes and capacity-based sizes remain warning-only AVM
adaptations. Arbitrary `.codehash` receivers, including nonzero literals outside
the recognized precompile convention, are compile errors. Direct self hashes
empty code during construction. No canonical-calldata or memory-model redesign
was included. See [EVM divergences](../../EVM_DIVERGENCE.md).

The new tests cover both sequencing modes and ABI profiles, constructor
inspection, receiver effects, runtime self aliases, zero/prefix validation,
literal application ids, actual approval-program bytes/hashes and diagnostics.
All **13 new cases pass**; final focused verification is **16 passed**.
Independent solc 0.8.34/PyEVM checks pass **20 portable assertions** (10 per
codegen). AVM program bytes/hashes are compared with algod's actual program,
not with EVM bytecode identity.

Final full suite: **2,131 passed, 1 failed, 100 xfailed, 39 xpassed** (2,271
total), in 632.40 seconds. Case-by-case comparison confirms all **2,258 prior
outcomes are unchanged**, all 13 new cases pass, and none were removed. The
sole failure is still `test_dce_reverting_subexpr_literal_folds`, the known
Puya DCE issue described above. Native CTests pass **21/21**, standard-library
runtime tests **20/20**, and chainwide harness units **91/91**. The first full
run caught a regression involving an unused `.transfer` member reference;
its prior handling was restored before this complete rerun.

Final compiler SHA-256:
`76af14c939542aa65f492ad473706cd9d60c9ccaa3564bc67865f4d7f8b7fa92`.
This item alone removes **85 total lines / 30 code lines** from `src/`, relative
to the preceding 1–7 checkpoint. `src/builder` contains **47,945 code lines**
(cloc 1.90, excluding comments and blanks).

Evidence: `/tmp/puya-sol-address-metadata-20260910.sLp6IJ/`, particularly
`semantic-verified.xml`, `semantic-verified.log`, `semantic-comparison.json`,
`focused-verified.xml`, `oracle.log`, `ctest-verified.log`,
`stdlib-verified.xml`, and `chainwide-verified.xml`. Earlier failed runs are
retained for diagnosis. The full run used `PUYASOL_LOCALNET_RESET=0` and the
preceding checkpoint's content-addressed `semantic-cache`; no ledger reset,
dependency change or marker change was made. Additional calls-audit probes in
the evidence directory identify open issues; they are not passing regression
tests or fixes included in this batch.

### Assembly audit (all nine items) — 2026-09-11

This working-tree batch uses solc's 256-bit constant and side-effect facts for
Yul arithmetic, operand capture and operation-specific offset conversion. It
separates calldata from scratch memory, checks complete access extents, preserves
partial and cross-page copies, shares Yul application-call transport and
precompile buffer handling, and reuses the canonical ABI codec and solc storage
layout facts. ARC4 field offsets remain target-owned. Outlined Yul functions now
share an immutable context and construct fresh lowering frames; obsolete memory
self-stores are removed. There is no general memory-model or terminating-Yul
calling-convention redesign. See the [bounded-buffer policy](../../EVM_DIVERGENCE.md#bounded-yul-buffers-and-precompiles)
for retained capacities, precompile restrictions and high-level-call differences.

Thirty-two new parameterized cases cover both Solidity sequencing modes, ABI
profiles and storage layouts. They exercise word signedness, read/write order,
full-width constants, distant calldata offsets, empty ranges, page boundaries,
exact output windows, return-data sharing, packed fields, nested ABI values and
nonzero BN254 pairing products. Independent pinned-solc/PyEVM oracles passed
**176 checks** across both codegens (54 words/order, 102 precompile buffers,
8 layout, 2 receive, 10 nonzero pairing checks).

The preceding frozen calls checkpoint had **2,178 passed, 9 failed, 100 xfailed,
39 xpassed** (2,326 cases). Eight of those failures are fixed here:
`abi.encodeCall` now retains canonical EVM selectors/results even on the ARC4
transport fast path, and packed signed deferred-constructor values select their
low declared-width bytes from the native carrier. Tests also now distinguish
`receive()` from `fallback()` for empty Yul call data, independently confirmed
against solc, and explicitly provide the ten-box array-copy fixture's post-init
resource budget. No value assertions were relaxed. CLI gas policy tests now
acknowledge `gas()` because its previously skipped operand is actually evaluated.

Final compiler SHA-256:
`a94c1487d6b2d1226a2e0e3f4cd20c679187442ef434761ce8baabbffbec681a`.
Focused LocalNet checks pass **49/49**, AVM stdlib **20/20**, native CTests
**21/21**, and offline harness/chainwide units **104/104**. The final full
semantic/harness run completed in **830.91 seconds** with three workers:
**2,218 passed, 1 failed, 100 xfailed, 39 xpassed** (2,358 cases).
Case-by-case comparison confirms eight baseline failures became passes,
the other 2,318 prior outcomes are unchanged, all 32 new cases pass, and
none were removed. The sole failure remains
`test_dce_reverting_subexpr_literal_folds`, the pinned Puya DCE/divide-by-zero
bug described above. This is not an all-green suite or an accepted divergence.
No expectation markers were changed. The compiler hash stayed fixed throughout.

Source code lines, including new files and excluding comments/blanks (cloc):
`src/` **53,434 → 52,377 (−1,057)**; `src/builder/yul/`
**8,434 → 7,229 (−1,205)**. These are relative to the dirty pre-assembly
snapshot, not to branch HEAD, which also predates the expression/calls work.

Generated approval sizes are a separate measurement. With identical cached
historical inputs and profiles, PLONK is **16,624 → 14,033 bytes** and Groth16
is **8,182 → 8,820 bytes**. Initial buffer-correctness changes grew both programs;
sharing checked offset, padded-read and exact-range routines recovered that
growth for PLONK but leaves a 638-byte increase for Groth16. Honk still fails
the pinned backend's signed branch-displacement encoding on both baseline and
final compilers. These are compile-only measurements, not proof replay or
deployment certification.

Evidence: `/tmp/puya-sol-assembly-20260911.2PlqIp/`, including
`build-stage5-repaired.log`, `focused-stage5-retry.xml`, `native-stage5.log`,
`semantic-final.xml`, `semantic-final.log`, `outcome-comparison.json`,
`stdlib-stage5.xml`, `offline-stage5.xml`, `historical-baseline/summary.json`,
`historical-stage5/summary.json`, oracle scripts/logs and cloc reports.
Earlier partial/failed runs are retained. The resumed build repaired four
empty object files left by interruption; the initial stage5 focused attempt
had KMD setup errors, not runtime outcomes. Only the stopped KMD service was
started, with its existing configuration. `PUYASOL_LOCALNET_RESET=0` remained
set; there was no ledger reset, dependency change, CI change or marker review.
At that checkpoint, the codec/contract/itxn audit was discussion-only; its
subsequent implementation is recorded below. `memory_redesign.md` remains
untouched and untracked.

### Codec, contract and inner-call checkpoint — 2026-09-11

The nine approved boundary changes share scalar/slot facts, precompile byte
algorithms, canonical AVM intrinsic declarations, getter projections, creation
reachability, reference resolution, calldata reconstruction and return-data
publication. Full application addresses are validated before compacting them.
Only prefixed return records are returndata; ordinary void-method events are
not, and explicit Yul return bytes remain observable. Mixed fresh/existing
modifier arguments preserve the selected reference without a memory-model change.

The frozen pre-ABI compiler was
`6e7afe54ab3669ad1293807f4f06053f1daeea052ed2f798c33483fa9c9c322c`.
Its completed semantic/harness run had **2,264 passed, 3 failed, 100 xfailed,
39 xpassed** (2,406 cases including 13 harness units, 1,439.33 seconds).
Besides the known Puya DCE failure, it exposed
an obsolete pairing-length expectation and the timing-sensitive `Round - 2`
seed mapping. The pairing test now accepts every whole 192-byte pair count
and passed a targeted rerun; the seed mapping is fixed in the next batch.
Those reruns are not substituted into the full-run totals.

Code-only `src/` size was **52,377 → 51,924 (−453)**. Evidence is retained in
`build/boundary-validation.Z1Rx57/`, including the full JUnit report and
98 solc/PyEVM boundary/reference oracle checks across legacy and via-IR.

### ABI and environment intrinsics — 2026-09-11

All nine approved findings are implemented. Packed encoding shares solc-derived
width/alignment facts and the canonical word codec, including UDVTs. Decode
operands are captured once; selector folding preserves receiver/branch effects.
Decode accepts solc's bounded unaligned offsets while retaining bounds and
scalar-padding checks. Raw literals retain their bytes, including non-UTF8
values; conditional literal branches follow solc's mobile-type rule.

Intrinsic dispatch uses solc `MagicType`, so locals named `block`, `msg` or
`tx` no longer collide with builtins. The obsolete `IntrinsicMapper` files,
string-based ABI dispatch, result wrappers and duplicate capability checks are
removed. Calldata source selection is shared and explicit; xchain claims are
excluded, short selectors are padded, and direct constructor reads are empty.
The opted-in Solidity/Yul seed mapping uses `FirstValid - 1`, with a warning
that this predictable, caller-selectable seed is **not secure randomness**.
See [the transport/seed conventions](../../EVM_DIVERGENCE.md#block-seed-and-calldata-conventions).

Aggregate decoder helpers share one per-contract registry. Fixed-layout arrays
preallocate buffers and use typed indexed writes; dynamic tails retain ARC4
rebasing. Arrays of 256-bit integers or `bytes32` use a type-proven word copy;
addresses and function pointers do not. One/two-element fixed arrays remain
unrolled. These choices were measured: general small-word loops save code but
can cost more opcodes, so this is not an unconditional runtime improvement.

Final compiler SHA-256:
`b1387784eb122126464d86e02b042fea305d6eeaea5df5cd16ac2d09c0a5178d`.
Focused ABI/fallback runtime tests pass **47/47**, native CTests **21/21**,
AVM stdlib **20/20**, offline harness/historical units **104/104**, and the
independent solc 0.8.34/PyEVM oracle **104/104** across legacy and via-IR.
The fresh complete semantic run finished in **936.02 seconds** with three
workers: **2,273 passed, 1 failed, 100 xfailed, 39 xpassed** (2,413 cases).
The sole failure is the known pinned-Puya
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(0)` returns zero instead
of reverting. This remains an open backend bug, not an accepted divergence
or an all-green suite.

Case-by-case comparison confirms the two other baseline semantic failures now
pass, the other **2,391 existing semantic outcomes are unchanged**, all
**20 added cases pass**, and no semantic cases were removed. The baseline's
13 harness units all pass in the separate final 104-test offline run; they
are not silently added to the new full semantic totals. The compiler hash
remained unchanged throughout the complete run.

Code-only size, excluding comments/blanks: `src/` **51,924 → 51,299 (−625)**;
`abi/` **1,712 → 1,182 (−530)**, and the 40-code-line intrinsic mapper is gone.
Same-source cached historical compilation produces PLONK SP1Verifier
**13,982 → 13,857 bytes** and Groth16 SP1Verifier **8,718 → 7,909 bytes**.
These are compile/size measurements, not historical replay or production
deployment certification. LocalNet was not reset; dependency pins, CI and
expectation markers are unchanged. `memory_redesign.md` is untouched/untracked.

Evidence is retained in `build/abi-fixes.zUDpmO/`: final compiler, build/native
logs, focused/full/stdlib/offline JUnit reports, oracle script/log, cloc reports
and before/after array size/opcode measurements. Earlier failed and partial
runs are retained separately, including the packed raw-literal regression
found during the first full attempt and corrected before the final run.

### Storage refactors — 2026-09-11

All eight approved storage findings are implemented. Fixed-array copies use
solc element/layout facts: recursive aggregate copies retain nested dynamic
contents, scalar word copies mask padding/tails, and self-copy is a no-op.
Named-layout copies rebuild ARC4 offsets for fixed arrays of dynamic elements.
Inherited immutables have distinct physical identities and their named cells
are included in slot-mode ARC-56/schema allocation. Sparse element strides
retain solc's full width instead of an unsigned-int implementation limit.

Storage paths share logical bounds, single evaluation and checked AVM narrowing.
Projection traversal and explicit box lifecycle facts replace duplicated shape
tests. Runtime planning uses solc's reachable callables, including modifiers;
transient declarations alone no longer require persistent sparse storage.
Unused layout data, default-value forwarding and duplicate copy code are gone.
Actual AVM encoded sizes drive capacity checks. At this checkpoint placement
changes were opt-in; encoded-size placement became the named-layout default
on 2026-09-12 and the flag was removed. See
[compatibility and retained copy limits](../../docs/storage-format.md#named-cell-placement-policy).

Final compiler SHA-256:
`729d1811311fbfa2007c627ae5b1bd6f7602a2536008ebb761397bef3d4b0bfd`.
The complete semantic run selected `tests/` with three workers and finished
in **1,297.16 seconds**: **2,306 passed, 1 failed, 100 xfailed, 39 xpassed**
(2,446 cases). The sole failure remains the pinned-Puya DCE bug
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(0)` does not revert.
All **2,413 baseline outcomes are unchanged**, all **33 added cases pass**,
and no cases were removed. This is not an all-green suite.

The focused storage/array/getter run finished with **360 passed, 4 xfailed,
1 xpassed**, no failures. Native CTests pass **21/21**, AVM stdlib **20/20**,
offline harness/historical units **104/104**, and independent solc 0.8.34/PyEVM
reference checks **48/48**, covering legacy and via-IR. Compiler and test-input
hashes remained unchanged throughout the full run.

Code-only counts exclude comments and blank lines: `src/` **51,299 → 51,053
(−246)**, `src/builder/` **44,601 → 44,341 (−260)**, and `storage/` **2,247 →
2,079 (−168)** across 20 storage files. The unused-assembly/transient fixture
shrinks **585 → 474 bytes**. At this checkpoint the legacy-placement fixture
remained byte-for-byte identical at **1,558 bytes**; encoded-size placement
produced **1,536 bytes**.
Cached PLONK and Groth16 SP1 verifiers remain byte-for-byte identical at
**13,857** and **7,909 bytes**. These are compile/size checks, not replay or
deployment certification. The initial sandboxed historical compiler-service
attempt stalled; the completed measurement used the one-shot backend.

Evidence is retained in `build/storage-refactors.NtmHTk/`: frozen compiler,
build/native logs, focused/full/stdlib/offline reports, outcome comparison,
oracle script/log, input hashes, cloc reports and bytecode measurements.
Earlier failed/stopped attempts are retained separately. LocalNet was not
reset; backend pins, CI and expectation markers are unchanged.
`memory_redesign.md` remains untouched and untracked.

### Default named-cell placement — 2026-09-12

Encoded-size placement is now the default outside EVM-slot storage; the
placement flag has been removed. Reference transport uses the same actual
AVM encoding facts instead of solc's conservative EVM-slot upper bound.
Struct types used as mapping values retain box-backed roots so their shared
box-key reference representation remains consistent. EVM-slot placement is
unchanged. Existing named-layout applications may require explicit migration;
see [storage compatibility](../../docs/storage-format.md#named-cell-placement-policy).

Final compiler SHA-256:
`7b781f8dd3a571dd09a40eba591cb1a0bc7c6faa4c4f012e18353e91f168794a`.
The full `tests/` run with four workers finished in **1,399.54 seconds**:
**2,326 passed, 1 failed, 100 xfailed, 39 xpassed** (2,466 cases). The sole
failure is the unchanged pinned-Puya DCE bug
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(0)` does not revert.
All **2,441 retained baseline outcomes are unchanged**. Four old placement
cases and the old flag-conflict case were intentionally replaced; all **25
added/replacement cases pass**, for a net increase of 20. No expectation
markers were changed, and this is not an all-green suite.

Focused storage facts pass **53/53**, native CTests **21/21**, AVM stdlib
**20/20**, offline units **104/104**, and solc 0.8.34/PyEVM reference checks
**54/54**. Coverage includes both ABIs, legacy/via-IR, named/slot storage,
exact schema allocation, the 128/129-byte placement boundary, mutation,
getters, delete, library/free references and mapping-valued struct roots.
The boundary fixture's bytecode and schema are identical in four comparisons:
new named default versus the former opt-in, and slot mode versus the previous
slot mode, each with both ABIs. Compiler and test-input hashes stayed fixed.

Code-only counts: `src/` **51,053 → 51,038 (−15)**, `src/builder/`
**44,341 → 44,340 (−1)**, and `storage/` **2,079 → 2,081 (+2)**.
Evidence is in `build/storage-default.kcRAgt/`, including frozen compilers,
JUnit reports, the per-case comparison, oracle checks and emission comparisons.
The concurrent top-level builder/runner audit had separate scratch probes and
unimplemented findings at this checkpoint; they are not part of these totals.
LocalNet was not reset; backend pins and CI are unchanged.

### Top-level builder and runner fixes — 2026-09-12

The nine audited items are implemented. Reference-boundary plans now cover
private and internally called public methods, follow declaration-ID alias
facts, and distinguish local rebinding from referent mutation. Original entry
references survive local rebinding; named return parameters retain their
initialization, and mutation summaries use the actual solc host context.
This is a repair of the existing mixed representation, not the deferred
whole-memory redesign or a claim of complete memory-alias support.

The name-based `efficientKeccak256` body substitution is gone. LogicSig roots
use the bundled declaration identity and a unique resolved entry, retaining
reachable helpers. Event selectors and logs share their encoding types, with
declared argument conversions and delayed encoding of mutable references;
EVM selectors still use solc's canonical signature. All portable library ABI
roots are emitted; host-only reference interfaces diagnose the missing
standalone artifact explicitly. Existing event/topic divergences remain.

Orchestration reuses the analyzed contract/function inventory. Returned roots
retain their WType arena; naming and contract emission have scoped lifetimes;
target choices are separated from derived storage facts. Source spans use
their owning imported file and solc positions. The direct-exec runner now
distinguishes launch failures and supports caller-controlled cancellation or
deadlines with process-group cleanup, without imposing a CLI timeout.

Final compiler SHA-256:
`6e53d707e6c64cf2b0d4a5a5d9ff5326e79848ca5c0f6ce37aece55d82eed321`.
Full semantic validation: **2,349 passed, 1 failed, 100 xfailed, 39 xpassed**
(2,489 cases), **1,075.32 seconds**. All **2,466 retained baseline outcomes
are unchanged** and all **23 new cases pass**. The sole failure remains
`test_dce_reverting_subexpr_literal_folds`, the known pinned-Puya DCE bug;
this is not an all-green suite. No expectation markers were changed.

The expanded focused run passes **82 tests with 2 expected failures**;
native CTests pass **22/22**, AVM stdlib **20/20**, offline units **104/104**,
and independent solc 0.8.34/PyEVM checks **50/50** across legacy/via-IR.
Compiler and source/test-input hashes stayed fixed throughout the final run.
Earlier failing attempts and their corrections are retained as separate evidence.

Code-only counts: `src/` **51,038 → 50,966 (−72)**; `src/builder/`
**44,340 → 44,199 (−141)**. Evidence: `build/builder-runner.aNBDlP/`.
LocalNet was not reset; backend pins and CI are unchanged.
`memory_redesign.md` remains untouched and untracked.

### Loose sol-ast fixes and cleanup — 2026-09-12

All nine approved items are implemented. Local bindings now belong to each
emitted function frame, with explicit scoped modifier/constructor sharing.
Member dispatch uses solc receiver types and declarations; function-address,
selector and unused-option projections preserve evaluation effects. Expression
locations retain their complete solc source identity.

Storage-pointer shortcuts consume one cached, exact solc type/layout proof
and the normal named/bound argument sequencing. Unproven shapes keep their
call bodies. Prepared solc/Yul effects propagate through the existing host-aware
call graph and reference-mutation summaries. Slot reads, writes and clears share
declared-type leaf rules, including fixed arrays, packed account auxiliary
words, dynamic bool packing and nested-array tail clearing.

ResolvedLValue has a typed destination and reusable source classification;
it freezes an address without permanently snapshotting the container's contents.
Delayed writes reload the container before changing the selected field. Small
move-order, pinning, fact-lookup and stale-comment cleanup is included. This
repairs the existing representation; it does not implement the deferred memory
redesign or change the accepted staticcall policy.

Final compiler SHA-256:
`2e4dde55f3a00d0a715d7b36013f4a9ac5dbc23d21224c9425bca6a0e4e07d79`.
Final full semantic run: **2,381 passed, 1 failed, 100 xfailed, 39 xpassed**
(2,521 cases), **347.68 seconds**. All **2,489 retained baseline outcomes
are unchanged**, and all **32 new cases pass**. The sole failure remains
`test_dce_reverting_subexpr_literal_folds`, the known pinned-Puya backend bug;
this is not an all-green result. No failure markers or backend pins changed.
Native CTests pass **22/22**, offline units **104/104**, AVM stdlib **20/20**,
and independent solc 0.8.34/PyEVM reference checks **64/64**.

The first full run exposed two assertions expecting the old fixed-array cap
diagnostic. The compiler still rejected length 258 at the unchanged 64-element
limit; only the expected message was updated. All four conversion-loop checks
then passed. A subsequent full repeat encountered a LocalNet outage; its
connection-failure totals are preserved separately, not counted as successful
validation. After Algod/KMD recovered, the final full run above completed on the
same compiler. Frozen compiler/source/test/config hashes were verified. No
LocalNet restart or ledger reset was performed.

Code-only counts: `src/` **50,966 → 50,881 (−85)**; `src/builder/`
**44,199 → 44,114 (−85)**. One packed-struct/nested-array measurement shrinks
approval code **761 → 706 bytes** and simulation budget **667 → 576**, with
identical results for all three inputs. Concurrent compile timings were noisy;
no universal size/runtime improvement or compile-speed claim is made.

Evidence: `build/sol-ast-fixes.5PjauP/`, especially `semantic6.log/xml`,
`outcome-comparison6.log`, frozen manifests, oracle logs and measurement scripts.
The subsequent `proxies-audit.md` and `builtin-audit.md` in that directory are
read-only findings, not additional implemented changes. The worktree remains
uncommitted; `memory_redesign.md` is unchanged and untracked.

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
