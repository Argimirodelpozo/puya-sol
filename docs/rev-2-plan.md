# rev-2: sol-types and storage implementation plan

Status: F1–F9 and R1–R3 are implemented and verified with no new full-suite
failures. Final results are 1,847 passed, 1 failed, 101 xfailed and 39 xpassed;
all 19 native tests and 98 focused regressions pass. The sole failure remains
the known Puya DCE divide-by-zero bug, so the suite is not fully green.
F3/F8 use the approved fresh-deployment-only compatibility boundary; F6 uses
the approved native-address transient shadow policy. Documentation retirement
and branch publication are the remaining handoff steps.

Branch: `rev-2`, based on `3b9a82d8e46c4ca99873b4fb1d7239d1ed50771b`.
Scope: all nine correctness findings from the sol-types/storage audit, plus
the three associated refactoring opportunities. Finding numbers below retain
the audit's numbering. This is the working checklist for the branch; update it
with implementation commits and actual test evidence as work lands.

## Source of truth and boundaries

Solc is authoritative for Solidity semantics. Prefer facts already computed by
the pinned compiler over reconstructing them from source spelling, AWST shapes,
declaration names, or estimates. Consult its implementation when an API's meaning
or preconditions are unclear; compare against its generated code when needed.

| Question | Authority to consume |
|---|---|
| Conversion legality, widths, signedness, literal kinds | Annotated solc source/target types, conversion predicates, `ConstantEvaluator`, and typed literal facts |
| Type identity | Solc `Type::identifier()` / `richIdentifier()`, with explicit representation normalization |
| Persistent and transient logical placement | `ContractType::linearizedStateVariables(DataLocation::Storage/Transient)` |
| Struct offsets, array shape and storage stride | `StructType::storageOffsetsOfMember()`, `ArrayType::length()` / `isDynamicallySized()`, and solc storage-size facts |
| Getter parameters and bounds semantics | Solc's generated getter function type and the indexed array's annotated type |
| ARC4 byte sizes and AVM physical placement | Target-specific encoding rules applied to solc facts; EVM slot/ABI sizes are not ARC4 payload sizes |

Primary implementation references are the pinned
[solc types API](../solidity/libsolidity/ast/Types.h) and
[implementation](../solidity/libsolidity/ast/Types.cpp), not an independently
maintained approximation of Solidity's rules.

Keep these boundaries throughout:

- Preserve supported AVM-native representations, including 32-byte addresses.
  Separate them from Solidity's logical slot/word representation; silently
  truncating native values is not a correctness fix.
- Do not call solc ABI predicates on types outside their supported domain,
  such as aggregates containing internal function pointers or mappings.
- Unsupported target capacities must produce clear diagnostics, not overflow,
  guessed defaults, assertion failures, or new silent divergences.
- Keep the existing warning-only staticcall policy. The pinned Puya DCE
  divide-by-zero failure remains a bug, not an accepted divergence.
- No CI work, XPASS/xfail reclassification campaign, dependency updates,
  unrelated payment changes, or ledger migration is part of this branch.

## Implementation sequence

Each row is a checkpoint, split into focused commits where useful. Pair each
fix with its regression coverage; establish solc expectations before changing
the lowering. Refactors must remove their superseded paths, not leave a second
source of truth alongside them.

| Step | Work | Dependency / completion gate |
|---|---|---|
| 0 | Record baseline; establish focused test/oracle cases | Exact root, solc, Puya, EVM revision and test commands recorded |
| 1 | F2 constants, F4 getter bounds, F7 transient initialization | Independent fixes and their focused tests |
| 2 | F9 checked size/capacity analysis | Shared arithmetic ready before storage placement/default changes |
| 3 | F1 + R1 aggregate conversion consolidation | Conversion selection is effect-free; evaluation-once regressions pass |
| 4 | R3 storage binding facts, then F8 dynamic classification | Separate behavior-preserving descriptor refactor from placement fixes |
| 5 | F6 transient representation and shared codec | Canonical logical layout; native representation preserved explicitly |
| 6 | F5 lazy mapping defaults | Uses checked sizes and explicit storage lifecycle facts |
| 7 | F3 unambiguous mapping-holder encoding | Persisted-key compatibility decision approved before format switch |
| 8 | R2 canonical type interning; finish duplicate-path cleanup | Representation identity and recursive-type tests pass |
| 9 | Full verification, documentation and branch handoff | Results reported honestly against exact tested revisions |

## Correctness work

### F1 — Aggregate widening must evaluate each source once

- [x] Fix the speculative widening path in
  [SolAssignment.cpp](../src/builder/sol-ast/exprs/SolAssignment.cpp): classify
  whether the conversion is supported before emitting source bindings or loops.
- [x] Pin effectful source elements before using them for both sign inspection
  and value construction. Cover static-to-static, static-to-dynamic and
  dynamic-to-dynamic integer widening through R1.
- [x] Use solc element types for signedness and widths, including supported
  wrappers; do not infer signedness from ARC4's unsigned storage representation.
- [x] Test supported assignments, initializers, arguments and returns, with
  signed/unsigned boundary values and effectful producers, in both storage modes.

Acceptance: solc-valid conversions compile, values match the solc oracle, and
conversion lowering adds no repeated source evaluations. Tests must not turn
unspecified sibling-expression evaluation order into a language guarantee.
The audit found repeated calls in emitted TEAL, and a separate static-array
encoding failure in the default storage mode; cover both paths.

### F2 — Encode assembly constants from typed solc facts

- [x] Replace textual `0x` detection in
  [SolcConstFold.cpp](../src/builder/sol-types/SolcConstFold.cpp) with solc literal
  kind/type information. Use `ConstantEvaluator` wherever it supports the value.
- [x] Preserve fixed-bytes alignment and right-padding through constant chains
  and widening; share the typed value-to-EVM-word rule rather than undoing and
  reapplying shifts based only on declaration width.
- [x] Compare high-level and assembly reads against solc for fixed-bytes chains,
  text beginning with `0x`, hex/string literals, and existing numeric/bool/address
  constant cases. Retain explicit conversion distinctions.

Acceptance: identical typed constants have the same canonical word as solc.
The audit confirmed disagreement between emitted TEAL and solc-generated IR.

### F3 — Make mapping-holder paths unambiguous

- [x] Replace unseparated root/member-name concatenation in
  [MappingPrefix.cpp](../src/builder/sol-ast/MappingPrefix.cpp) with one structured
  holder-path representation and one unambiguous encoder.
- [x] Derive logical identity from solc-resolved declarations, root slots and
  member offsets. Encode segment kinds/boundaries explicitly where paths remain
  structured. Compilation-local AST IDs must not become persisted keys.
- [x] Route direct accesses, storage aliases, internal/library reference
  parameters, nested mappings and getters through that same encoding rule.
  Keep ARC-56 descriptions and test-harness key construction consistent.
- [x] Test distinct holder paths for isolation and equivalent alias paths for
  identity; preserve existing dynamic mapping-key hashing regressions.
- [x] Resolve and document the persisted-key compatibility gate below before
  changing the default physical format.

Acceptance: holder-path identity is independent of ambiguous name concatenation,
and every consumer agrees on the physical key. The remaining audit finding is
holder-path framing, not the dynamic string/bytes key hashing already present.
This finding was source-traced, not runtime-confirmed in the audit.

The implementation separates a logical holder key from its serialized data
path. Key-only parameters/returns of mapping-containing aggregates require a
whole-box root (including a mapping-entry box); unsupported interior handles
diagnose instead of losing the parent data path. Direct updates, local aliases,
mapping-field references and passing the whole enclosing aggregate are covered.
Solc-confirmed nonrecursive single-struct wrappers at coordinate `(0, 0)` with
the same storage extent are represented transparently: their sole member keeps
the same data path and holder, without an extra ARC4 wrapper header. This allows
whole-box references through wrappers while preserving nominal type identity.

### F4 — Validate getter indices before narrowing

- [x] Replace unchecked index narrowing in
  [PublicGetterBuilder.cpp](../src/builder/contract/PublicGetterBuilder.cpp) with
  the existing checked-index conversion or a full-width comparison followed by
  a proven-safe cast. Consume solc's getter parameter and array-length facts.
- [x] Cover flat arrays, nested arrays, arrays on either side of mapping levels,
  and struct-array getters. Preserve the already full-width slot-mode path.
- [x] Extend getter regressions with valid boundaries, empty arrays, and invalid
  full-width indices; compare getter behavior with explicit indexed reads.

Acceptance: an invalid Solidity index cannot become valid through a narrowing
cast. The audit traced this ordering issue in default-layout getter generation;
runtime coverage now includes flat/nested arrays and indices before/after mappings.

### F5 — Preserve zero defaults for absent large mapping values

- [x] Remove the assumption in
  [StorageMapper.cpp](../src/builder/storage/StorageMapper.cpp) that every large
  fixed-width box has already been created. Use explicit eager/lazy lifecycle
  information from R3, including after deletion.
- [x] Handle absent lazy storage at the requested element/window level, returning
  the type-correct zero/default without materializing an oversized stack value.
  Reuse this behavior for explicit reads and generated getters.
- [x] Keep bounds validation independent of box existence. Reads must not create
  boxes; first partial writes must create a valid backing representation.
- [x] Test unwritten, written and deleted values; scalar and aggregate elements;
  stack-capacity boundaries; and supported multi-box boundaries. Reject any
  unsupported shape explicitly rather than treating it as initialized.

Acceptance: valid reads from unwritten mapping entries return Solidity defaults
without storage mutation or AVM capacity violations. The audit observed a bare
box extraction with no existence/default handling in generated TEAL.

### F6 — Unify transient scalar representation and packing

- [x] Consume solc's transient logical layout, rather than independently
  reconstructing inheritance order and packed offsets in
  [TransientStorage.cpp](../src/builder/storage/TransientStorage.cpp).
- [x] Resolve UDVT underlying types consistently. Distinguish logical Solidity
  width from native physical width for addresses, contract values and function
  pointers using the same representation facts as other storage backends.
- [x] Reuse [SlotWordCodec](../src/builder/storage/SlotWordCodec.h) for canonical
  word packing/unpacking. Keep any native-width/shadow adaptation explicit;
  blindly applying a 20-byte codec to a native address is not sufficient.
- [x] Test direct and wrapped values, signed widths, bytes alignment, inheritance,
  neighboring packed values, high-level/Yul agreement, and transient lifetime
  across supported call boundaries in both ABI profiles.

Acceptance: wrapped and unwrapped values round-trip consistently; assembly uses
solc's logical slots/offsets; all supported native address bits survive. The
audit confirmed a direct/wrapped address width mismatch in generated TEAL.
Any unavoidable new non-exact behavior requires a separate policy decision.

The native-address adaptation was approved on 2026-09-06: typed reads/writes
preserve all 32 address bytes in separate scratch storage, while `tload`/`tstore`
use solc's logical packed word. A raw `tstore` clears the extra native-only
address bytes for declarations in the affected slot. This agrees with solc
within its 160-bit address domain but changes raw-word round trips for full-width
AVM addresses. No persistent cells or ledger migration are involved. Do not
truncate native typed addresses; the adaptation and canonical transient-layout
switch are implemented and covered by the tests in checkpoint 9.

### F7 — Exclude transient declarations from persistent initialization

- [x] Skip transient declarations in named-cell constructor initialization in
  [ApprovalProgramBuilder.cpp](../src/builder/contract/ApprovalProgramBuilder.cpp).
  Keep the actual transient-state initialization and lifetime handling intact.
- [x] Make initialization, storage declarations and ARC-56 schema generation
  consume the same storage-class facts; cover mixed persistent/transient and
  inherited declarations in both storage modes.
- [x] Add an exact-artifact-schema deployment regression, with no spare global
  cells supplied by the harness. Add only the focused harness support needed;
  do not silently change every existing deployment's schema policy.

Acceptance: transient-only contracts require no persistent cells and deploy with
the schema they advertise. The audit found persistent writes despite a zero-cell
ARC-56 schema; the harness currently reserves spare cells and can hide this.

### F8 — Classify dynamic aggregates containing internal functions correctly

- [x] Replace the blanket ABI-predicate bypass with a storage-shape query over
  valid solc facts: dynamic array size, base types and struct member types.
  Reuse safe solc predicates where applicable; keep recursive-type traversal
  guarded and do not invoke ABI encoding queries on non-ABI types.
- [x] Share the classification between storage placement and reference/handle
  passing in [StorageRefPointer.h](../src/builder/sol-ast/StorageRefPointer.h).
- [x] Preserve legitimate small fixed aggregates as fixed values. A dynamic
  array's one-slot solc storage head is not a bound on its serialized contents.
- [x] Test dynamic arrays, nested/fixed containers of dynamic arrays, structs,
  and fixed controls containing internal function pointers. Exercise mutation,
  reads, invocation and internal/library storage-reference paths where supported.

Acceptance: supported dynamic cases compile and execute with coherent placement
and handles, without solc assertion failures. The audit's dynamic function-array
probe was placed in app-global storage and failed backend compilation on mutation.
Classify any placement change under the compatibility gate before shipping it.

### F9 — Make encoded-size and layout arithmetic checked

- [x] Replace the ambiguous `int`/zero encoded-size result in
  [Arc4Defaults.cpp](../src/builder/sol-types/Arc4Defaults.cpp) with explicit fixed,
  dynamic, and unsupported/overflow outcomes. Distinguish an actual zero size
  from an unknown size. Use checked wide arithmetic or bounded comparisons.
- [x] Migrate aggregate totals, packed-bool sizes, box/page counts, blob selection,
  and placement estimates to those results. Model ARC4 sizes, not EVM ABI sizes.
- [x] Validate solc array lengths, member offsets and storage strides before
  narrowing in TypeMapper, StorageMapper and SlotHandleAccess. Keep logical
  slots full-width; bound only the target representation that requires it.
- [x] Test arithmetic boundaries as metadata-only native tests, without giant
  allocations. Add compile-time diagnostics tests for unsupported capacities.

Acceptance: oversized values cannot wrap into small/fixed representations, and
capacity diagnostics occur before allocation or invalid code emission. The audit
directly confirmed a large fixed-array encoded size wrapping to zero.

## Refactoring work

### R1 — One aggregate conversion route

- [x] Extend the existing
  [ConversionPlan](../src/builder/sol-types/ConversionPlan.h), not a parallel
  framework: select semantic conversion from solc types before emitting effects.
- [x] Share scalar integer widening across supported array shapes. Give emission
  explicit evaluation-once and loop-effect handling rather than speculative
  helper calls that mutate pre-effects and can then decline the conversion.
- [x] Route relevant assignment, initialization, argument and return consumers
  through it and remove duplicated signed/static/dynamic conversion branches.

Keep unsupported Solidity conversions unsupported; this is consolidation and
correctness work, not an expansion of the language's implicit conversion rules.

### R2 — Canonical, normalized solc type interning

- [x] Replace `toString(true)` plus recursive `declarationIdentitySuffix()` in
  [TypeMapper.cpp](../src/builder/sol-types/TypeMapper.cpp) with solc identifiers
  after deliberate representation normalization.
- [x] Specify which memory/storage/calldata distinctions are intentionally shared
  by the runtime representation and which must remain distinct. Raw identifier
  substitution alone is not a behavior-preserving change.
- [x] Preserve nominal identity for structs, enums, UDVTs and contracts, including
  nested types and function signatures. Test same-named declarations from distinct
  sources, recursive projections and all relevant data locations.
- [x] Retain valid recursive construction/caching behavior and test consumers
  that rely on interned WType pointer identity. Never use these compilation-local
  identifiers as a persisted-storage format.

Acceptance: equivalent representations intern consistently, distinct nominal
types do not alias, and the handwritten identity-suffix recursion is removed.

### R3 — Shared storage binding, lifecycle and representation facts

- [x] Extend the existing physical binding/runtime-plan model instead of adding
  another competing planner. Record declaration identity, storage class,
  logical layout, physical representation, and eager/lazy/default behavior.
- [x] Have initialization, reads/writes, reference passing, getters and schema
  generation consume the same facts. Runtime mapping entries still carry their
  own keys and existence state; declaration metadata alone cannot prove existence.
- [x] Replace lifecycle guesses based on raw AWST expression shape where the
  new facts apply. Reuse the canonical word codec for transient operations.
- [x] Remove redundant name/type arguments, unused name-index maps and duplicate
  default/packing logic once all affected consumers and tests are migrated.

Acceptance: the initial refactor preserves layout and behavior byte-for-byte;
intentional fixes are separate, test-backed changes. Do not remove backend
workarounds merely because they appear repetitive or reference stale documents.

Bindings, explicit declaration origins, fixed-width missing-box reads and
transient codec consolidation are implemented. The transient name index,
duplicate scalar codec and redundant read-type argument are removed.

## Persisted-storage compatibility gate

F3 changes physical keys. F8 and any revised placement estimates can move state
between global cells and boxes. Neither is a harmless refactor for an existing
deployment, even when the new behavior agrees better with Solidity.

- [x] Inventory affected layouts and compare representative pre/post manifests.
- [x] Propose an explicit format/version boundary and fresh-deployment behavior.
  Prefer stable identities derived from solc logical layout, not source spelling
  or AST numbering. Solc-compatible layout changes still require compatibility
  review when applied to existing contracts.
- [x] Obtain a decision before switching an existing persisted format: a breaking
  fresh-deployment-only release, or an explicitly versioned compatibility path.
  Do not silently reinterpret old keys or automatically read ambiguous legacy
  locations. Any migration of deployed state is a separate authorized task.
- [x] Update ARC-56 metadata, related tooling and documentation with the decision.

On 2026-09-06 the user approved a fresh-deployment-only format break. Existing
deployments must retain their original compiler/artifacts; no automatic legacy
fallback or state migration will be added. F3 affects default-layout holder identities,
their derived mapping boxes, and the encoded payload of transparent wrappers;
F8 affects default-layout dynamic aggregates
containing internal functions, moving their schema/initialization and reference
handling from named globals to boxes where required. Neither fix requires
changing EVM slot-mode keys. The implementation selects
[holder format 2](storage-format.md): a printable, versioned full-width solc root
coordinate and framed/tagged member, array and mapping hash steps. Names remain
artifact labels, not key inputs. ARC-56 describes exact roots instead of
pretending hash-derived entries are prefix maps. Checkpoint 10 records runtime,
metadata and manifest-comparison evidence for this boundary.

## Verification and definition of done

### Checkpoint 1 — constants, getter bounds and transient schema

The first implementation checkpoint passed all 16 native CTests and 42 focused
semantic/harness tests (two workers, 10.79 seconds). Report:
`/tmp/puyasol-rev-2-initial-expanded.xml`. The focused command selected
`test_sol_types_storage.py`, `test_getter_array_bounds.py`,
`framework/test_compile_cache.py`, and `framework/test_harness.py`, with
`PUYASOL_LOCALNET_RESET=0`. The earlier 26-case selection also passed the existing
inline-assembly constant-access tests. Constants and exact-schema deployments
cover both ABI profiles; getter/schema coverage exercises both storage modes.
An in-process solc 0.8.34/Cancun oracle confirmed the constant-word expectations.
The full semantic suite has not yet been rerun for this branch.

### Checkpoint 2 — checked sizes and capacity diagnostics

All 18 native CTests passed, including metadata-only size/overflow tests and
compile-only diagnostics for solc-valid but unrepresentable array sizes. The
focused array/storage/getter/UDVT/builder selection passed 247 tests, with 7
existing xfails and 4 existing xpasses, in 58.30 seconds; no markers changed.
Report: `/tmp/puyasol-rev-2-sizes.xml`. The full suite remains pending.

`EncodedSize` now distinguishes fixed (including zero), dynamic, packed,
unsupported and overflow outcomes. Every legacy integer-size consumer requests
a checked narrowing explicitly. Solc array lengths/member offsets/strides are
checked before conversion, and default encoding is bounded before allocation.
The redundant StorageMapper size forwarding API was removed. Builds use
`CCACHE_DISABLE=1` where the configured cache directory is read-only.

### Checkpoint 3 — evaluate-once aggregate conversions

All 18 native CTests passed. The expanded array/storage/getter/UDVT/constructor/
tuple selection passed 336 tests, with 23 existing xfails and 7 existing xpasses,
in 40.51 seconds. Report: `/tmp/puyasol-rev-2-conversions-verified.xml`.
The focused type/storage cases also passed independently (20 tests, 24.10
seconds; `/tmp/puyasol-rev-2-conversions-fixed.xml`).

One selected integer-array strategy now shares scalar widening for fixed/fixed,
fixed/dynamic and dynamic/dynamic storage copies. Selection emits no effects on
a declined match; accepted conversions bind their source once. Assignment,
tuple, boxed-aggregate, struct-field and constructor routes share this conversion
handling, removing roughly 500 net source lines of overlapping lowering.

The runtime fixture covers both storage modes, ABI profiles and codegen modes,
negative/positive boundaries, empty arrays, tuple/member stores, initializers,
and producer calls through internal argument/return paths. Solc permits array
element widening for storage copies, not arbitrary memory argument/return
conversions (`ArrayType::isImplicitlyConvertibleTo`); no language rule was
relaxed. A 257-element case exercises the fixed-array conversion loop in the
default layout. Slot mode retains its existing explicit 64-element whole-fixed-
array store limit, covered by a diagnostic assertion, not an xfail.

Expected results were confirmed with an in-process solc 0.8.34/Cancun oracle,
optimizer disabled and legacy pipeline. Intermediate runs exposed and fixed
previously bypassed slot-constructor and boxed-aggregate conversion routes;
those failures are resolved in the checkpoint result above.

### Checkpoint 4 — canonical type identity

All 19 native CTests passed. The focused array/struct/function/type-storage
selection passed 190 tests, with 5 existing xfails and 1 existing xpass, in
38.85 seconds. Report: `/tmp/puyasol-rev-2-type-identity-verified.xml`.

Value arrays and structs, including tuple components, normalize their location
through solc before using its canonical identifier. Callable parameter/return
locations and non-relocatable calldata slices remain intact. The handwritten
declaration-identity suffix traversal is removed. Native tests cover nominal
types from distinct source units, fresh equivalent solc objects, pointer/ref
forms, recursive projections, cache resets, callable signatures and slices.
The semantic copy/reference regression covers both storage modes and ABI
profiles; the expected result was confirmed by solc 0.8.34/Cancun, optimizer
disabled, legacy pipeline. An intermediate run caught an invalid attempt to
relocate calldata slices; that is fixed and covered by both test layers.

R2 was completed before the persisted-storage steps while their compatibility
decision remains pending.

### Full-suite checkpoint — b9fe763671

The fixed compiler at `b9fe763671` ran `framework/test_compile_cache.py`,
`framework/test_harness.py` and `tests/` with two workers and no LocalNet reset.
Result: **1,736 passed, 6 failed, 102 xfailed, 38 xpassed**, 291.33 seconds.
Report: `/tmp/puyasol-rev-2-type-checkpoint-full.xml`.

The known Puya `test_dce_reverting_subexpr_literal_folds` failure remains. Five
additional failures surfaced F9's premature capacity diagnostics:
`test_FixedFeeRegistrar`, the two compile-time ERC-7201 array-length tests,
`test_denominations_in_array_sizes`, and `test_denomination_array_layout`.
Solc logical layout and fixed `.length` facts do not require materializing the
entire array; those uses must remain available at full width. Actual unsupported
materialization/access still requires a clear diagnostic. F9 is not finally
verified until these cases are resolved and the full suite is rerun.

### Checkpoint 5 — contract-scoped storage bindings

All 19 native CTests and 281 focused semantic tests passed, with 11 existing
xfails and 1 existing xpass, in 64.90 seconds. Report:
`/tmp/puyasol-rev-2-bindings.xml`. Selection: storage, getters, inheritance,
immutable, structs, arrays, storage/type and parity regressions, remaining-review
regressions, and the existing declaration-identity/contract-dispatch tests.

Bindings now cache storage class, solc logical placement, native type, physical
key/kind and root initialization once per contract. They do not claim a mapping
entry exists. Initialization, schema and native read/write paths share these
facts; explicit box-origin metadata survives aliases without guessing from a
literal key. The unused backend name/type arguments and duplicate array-root
initialization classification were removed. The mapping-root placeholder and
existing preallocation policy are deliberately unchanged.

The new runtime fixture compiles opposite inheritance orders and standalone
bases together, checks independent state and delete/reuse, and deploys with exact
schemas in both ABI/storage profiles. Its stateful sequence was confirmed by
solc 0.8.34/Cancun (legacy pipeline, optimizer disabled). Native coverage checks
origin preservation through interleaved StateGet/reinterpret wrappers.

Complete AWST outputs compare byte-for-byte equal before/after for canonical
same-name storage, colliding aggregate names, array conversions and their slot
mode counterpart. Snapshots: `/tmp/puyasol-rev-2-bindings-before` and
`/tmp/puyasol-rev-2-bindings-after`. This includes app-state definitions/keys;
no persisted-format switch was made. Full-suite F9 failures above remain pending.

### Checkpoint 6 — full-width storage facts without whole-value materialization

All 19 native CTests and 12 focused semantic tests passed (9.14 seconds).
Report: `/tmp/puyasol-rev-2-huge-facts-verified.xml`. This includes all five
additional full-suite failures identified above, plus both ABI/storage profiles
for huge fixed lengths and assembly `.slot`, and full-width sparse array access.
Reads leave box names unchanged; sparse entries remain isolated and invalid
indices revert. Solc 0.8.34/Cancun confirmed the new layout and sparse-access
expectations. The ERC-7201 builtins instead rely on the pinned development solc
and their existing corpus expectations; 0.8.34 does not expose that builtin.

Declaration bindings retain canonical solc facts when no whole-value WType is
available. Actual value access/initialization still requests the strict checked
representation. Partial failed struct mapping cannot poison the type cache.
Sparse-runtime selection now checks the full solc storage span, not just the
declaration's starting slot. Assembly coordinates no longer map the entire
referenced array merely to expose its slot.

`FixedFeeRegistrar` previously had only a default-layout compile/deploy smoke
test; its huge sparse array cannot have a named-cell representation. The test
now asserts that capacity diagnostic and explicitly uses slot mode for deploy
and absent-record read assertions. No automatic backend switch or persisted
format change is introduced, and no xfail or assertion was weakened.

The full suite against `bd1956b410` passed **1,752 tests**, with **1 failed**,
102 existing xfails and 38 existing xpasses, in 311.41 seconds. Report:
`/tmp/puyasol-rev-2-storage-facts-full.xml`. The sole failure is the unchanged
Puya DCE divide-by-zero regression. There are no additional failures at this
checkpoint; this is not a claim that all semantic tests are green. The compiler
binary remained fixed for the complete run.

### Checkpoint 7 — lazy fixed boxes and bounded projections

All 19 native CTests passed. The focused lazy-storage runtime selection passed
8 tests in 21.24 seconds, covering both ABI profiles; report:
`/tmp/puyasol-rev-2-lazy-boxes-expanded.xml`. The final expanded selection passed
**16 tests** in 22.12 seconds, covering both code-generation modes as well as
both ABI profiles and four explicit diagnostic cases; report:
`/tmp/puyasol-rev-2-lazy-boxes-profiles.xml`. The broader array/struct/storage/
getter/function-type/regression selection passed **713 tests**, with 1 failed,
7 existing xfails and 1 existing xpass, in 200.83 seconds. Report:
`/tmp/puyasol-rev-2-lazy-storage-broad.xml`. The only failure is the unchanged
Puya DCE divide-by-zero bug; markers are unchanged.

Large fixed boxes remain addressable places until a bounded projection is read.
The shared read guard pins keys and indices, checks bounds before existence, and
defaults only the requested scalar/aggregate. Explicit reads, getters, memory
initializers, copies, returns and value arguments share this rule. Oversized
fixed-array reference parameters now carry a validated whole-box key rather than
depending on backend inlining of a whole-value argument. Interior large-array
references and unsupported mapping-value paging produce explicit diagnostics.

Partial writes create valid defaults, including after deletion. Large struct
field updates use projected lvalues instead of copying oversized siblings.
Paged reads/writes/getters/element deletes share checked page addressing; root
delete clears every page, absent reads and element deletes allocate nothing, and
the short final page is recreated at its exact size. Existing page keys are
unchanged. A missed declaration-origin factory for arrays containing mappings
was also corrected.

Tests cover 4 KB and 32 KB boundaries, nested bounds, signed/bool/bytes fields,
aliases, effectful sources, independent mapping entries, aggregate copies,
delete/reuse and paged boundaries. Expected new runtime sequences were confirmed
with solc 0.8.34/Cancun, optimizer disabled, legacy pipeline. The existing
recursive multi-box struct and direct compound-update regressions pass as part
of the broader selection. Slot mode retains its explicit 64-element whole-array
delete limit; the new large-array lifecycle fixture cannot run there and is
covered by a capacity diagnostic rather than an xfail.

The full suite against `b3f596cc11` finished with **1,768 passed, 1 failed,
102 xfailed and 38 xpassed**, in 353.52 seconds. Report:
`/tmp/puyasol-rev-2-lazy-full.xml`. The sole failure remains
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(uint256)(0)` returns zero
instead of reverting. There are no additional failures at this checkpoint, but
the suite is not all green. All 19 native CTests also passed in 1.93 seconds.
The compiler binary remained fixed throughout the full semantic run, using:

```bash
PUYASOL_LOCALNET_RESET=0 pytest framework/test_compile_cache.py \
  framework/test_harness.py tests/ -q -n 2 --tb=short \
  --junitxml=/tmp/puyasol-rev-2-lazy-full.xml
```

At this checkpoint the branch has 241 fewer net source lines than its base;
the bounded-read correctness work adds code while the earlier conversion,
identity and storage-binding consolidations remove duplication.

### Checkpoint 8 — dynamic storage shape without ABI predicates

The shared storage-shape query walks solc array/member facts with a recursion
guard. Internal functions remain fixed-size scalar values, but no longer hide
dynamic arrays in their enclosing aggregates. Placement and storage-reference
classification agree. Small fixed callback structs/arrays remain globals.

All 19 native CTests passed (2.59 seconds). The function/array/struct selection,
including 8 new runtime cases and 2 capacity-diagnostic cases, passed **164 tests**,
with 5 existing xfails and 1 existing xpass in 50.98 seconds. Report:
`/tmp/puyasol-rev-2-function-shape-broad.xml`. Runtime coverage exercises both ABI,
storage and code-generation profiles, exact schemas, callback invocation,
mutation, deletion/reuse, whole-box internal/library references and struct
references. Solc 0.8.34/Cancun confirmed the stateful expectations (legacy
pipeline, optimizer disabled).

The initial run exposed unsupported interior dynamic-array parameters: their
key-only handle lost the parent location and could corrupt the enclosing array
header. Those calls now diagnose the unsupported interior resize path rather
than passing the parent's box as if it were the whole array. Nested arrays and
struct-held arrays remain usable through direct updates; callers may pass the
enclosing struct as a reference. No existing test assertions or markers changed.

Representative pre/post AWST manifests are preserved under
`/tmp/puyasol-rev-2-format-before.eL6NrY/functions` and `functions-after`.
The four dynamic callback aggregates move from named globals to same-named boxes;
the two small fixed controls remain globals. A deferred-constructor flag is
added where array-box initialization requires it. This is an approved
fresh-deployment-only placement change, not an upgrade/migration path. EVM
slot-mode persistent keys are unchanged.

The subsequent regression/storage selection passed **563 tests**, with one
failure and two existing xfails in 192.31 seconds. Report:
`/tmp/puyasol-rev-2-function-shape-regressions.xml`. The only failure was the
unchanged pinned-Puya DCE divide-by-zero regression.

### Checkpoint 9 — canonical transient words with native address preservation

Solc's transient layout now supplies inheritance order, full logical slots and
packed offsets. Typed scalar reads/writes share `SlotWordCodec`; 32-byte native
addresses (direct, UDVT and contract values) keep their high bytes in scratch 99,
outside the logical word. Raw `tstore` pins its slot argument once and clears
only the corresponding native shadow. The reservation is safe even at the
maximum 88 memory pages and creates no persistent schema entries.

State function-pointer calls and qualified state-value reads now use the normal
storage dispatch, fixing transient callbacks that previously read absent globals.
The existing transient inline-assembly fixture's address/bool layout expectation
is restored from `(2, 0)` to solc's `(1, 1)`; no xfail/XPASS markers changed.

All 19 native CTests passed (8.98 seconds). The transient/variable selection
passed **35 tests** in 262.13 seconds; report:
`/tmp/puyasol-rev-2-transient-words-second.xml`. The expanded assembly/function/
variable/storage selection passed **193 tests**, with 8 existing xfails and
3 existing xpasses, in 359.64 seconds; report:
`/tmp/puyasol-rev-2-transient-words-broad.xml`.

Runtime cases cover both ABI, storage and code-generation profiles, exact zero
schemas, direct and wrapped addresses, signed widths, fixed bytes, internal and
external function pointers, contract values, inherited layout, packed neighbors,
raw word reads/writes, evaluation-once and reset across application calls.
Solc 0.8.34/Cancun confirmed logical layout and EVM-domain runtime expectations,
legacy pipeline with optimization disabled. Full native `Txn.sender` preservation
and shadow invalidation are asserted directly on AVM in both ABI profiles, not
claimed as an EVM byte-for-byte comparison. After removing unused metadata and
the redundant transient read-type argument, the final rebuilt checkpoint passed
all 19 native tests (4.42 seconds) and **59 semantic tests**, including public
getters, in 100.77 seconds; report: `/tmp/puyasol-rev-2-transient-cleanup.xml`.

### Checkpoint 10 — versioned solc-coordinate storage holders

One shared resolver retains logical holder identity separately from the
serialized data path. A reserved, printable 54-byte root encodes the full solc
slot/offset; framed SHA-256 segments distinguish struct members, array indices
and declared-type mapping keys. All descendant keys are 32 bytes. Root/member
names and AST IDs no longer participate in mapping-holder identity. Superseded
cursor/prefix reconstruction and the old generic mapping-key-layer helper are
removed; TypeMapper retains session-local solc aggregate provenance for aliases.

The initial 17-case runtime/metadata selection passed in 33.82 seconds; report:
`/tmp/puyasol-rev-2-storage-holders-second.xml`. Expanded coverage passed
**44 tests** in 44.10 seconds, including both ABI/storage/code-generation
profiles, exact-schema deployments, runtime box-key checks, reference lifetime,
full-width invalid indices and explicit interior-handle diagnostics. Report:
`/tmp/puyasol-rev-2-holders-extended-first.xml`. All 19 native CTests passed in
1.46 seconds, including solc-provenance/reset checks.

The first broader selection exposed two regressions and one stale format
assertion, alongside the known Puya DCE failure (396 passed, 4 failed,
6 xfailed, 2 xpassed; 136.21 seconds;
`/tmp/puyasol-rev-2-holders-broad-first.xml`). The two regressions are fixed:
reference-call pre-effects now precede returned-key binding, and child-constructor
placement classification no longer requests a physical key from the parent's
layout. The metadata assertion now verifies an exact format-2 mapping root,
not the removed, inaccurate prefix-map declaration. All three passed in the
44-test rerun; the subsequent full run is recorded in checkpoint 11.

Solc 0.8.34/Cancun, legacy pipeline and optimizer disabled, confirmed the new
stateful value/isolation/evaluation-once expectations and the validity of the
interior-reference fixtures. Slot mode executes those interior references;
default layout diagnoses the unsupported richer-handle requirement. Unsupported
shapes are not silently lowered to the parent box or a made-up child data box.
The existing nested library reference regression additionally runs through all
eight profiles and matches the solc oracle's 42/99/0/0 read sequence.

Pre/post AWST manifests under
`/tmp/puyasol-rev-2-format-before.eL6NrY/holders-format-fixture` and
`holders-format-after-second` show 14 mapping-containing roots across four
contracts switching keys, with placement unchanged. Five direct mapping roots
stop advertising their entries as prefix maps and instead describe the actual
root bytes placeholder. In that fixture, aggregate data representations, ordinary named cells
and constructor-pending flags are unchanged. Original/renamed declarations
have identical new roots, including separately deployed runtime box checks.
The `holders-slot-before` and `holders-slot-after` **complete AWST files compare
byte-for-byte equal**, not merely their state declarations. Existing artifacts
and LocalNet state were preserved; no legacy fallback or migration was added.

### Checkpoint 11 — holder reference coherence and transparent wrappers

The first combined full run recorded **1,829 passed, 3 failed, 101 xfailed and
39 xpassed**, in 394.10 seconds; report:
`/tmp/puyasol-rev-2-holders-full-first.xml`. Alongside the unchanged Puya DCE
failure, it exposed two regressions: EnumerableSet's single-member wrapper was
rejected as an interior reference, and mapping-local tuple assignment did not
update its runtime binding. Both are fixed and pass unchanged assertions.

The expanded holder selection now passes **51 tests** in 27.19 seconds, including
the two existing regressions; report:
`/tmp/puyasol-rev-2-holders-wrappers-third.xml`. All **19 native tests** pass in
1.42 seconds. New coverage exercises tuple swaps, conditional rebinding and
shadowed locals in both storage/ABI/code-generation profiles, plus transparent
wrappers through direct access, mapping and array elements, library calls,
anonymous/named reference returns, deletion and reuse. Expected stateful results
were confirmed by solc 0.8.34/Cancun, legacy pipeline, optimizer disabled.

Runtime storage handles use their registered, shadow-safe declaration bindings
for reads, assignment and assembly. Tuple RHS runtime keys/slots are pinned before
component writes. Reference-return planning now gives non-indexed
mapping-containing returns the same key representation as their callers and
implicit named returns. Reference-call effects precede the returned-handle
binding rather than using uninitialized argument pins.

Transparent wrappers retain distinct nominal WTypes but use the innermost
represented fields and holder. The solc recursive-type accessor, member offsets
and storage extent decide eligibility; recursive wrappers and slot-mode
representations remain unchanged. Access preserves the addressable expression
instead of emitting an invalid aggregate-to-aggregate AWST cast.

Pre/post manifests in `wrappers-format-before` and `wrappers-format-final`, under
`/tmp/puyasol-rev-2-format-before.eL6NrY/`, show the additional wrapper payload
normalization explicitly: `Wrapped` and `Deep`, including array elements, expose
`tag/items/values` directly instead of nested `inner` headers. The mapping root
describes its actual bytes placeholder rather than an entry type. Constructor
flags remain unchanged. Runtime box reads independently verify the documented
key derivation from solc root slots `0/3/6/7` and member slot `2`, omitting both
transparent wrapper ranks.

The next full run recorded **1,837 passed, 3 failed, 101 xfailed and 39 xpassed**
in 369.02 seconds (`/tmp/puyasol-rev-2-holders-full-second.xml`). The two additional
failures were `test_storage_return_facts` in both storage modes: assembly `.slot`
assignment still wrote the unmangled local name while reads used its registered
binding. Single- and multi-return Yul assignments now share the same storage-slot
redirect. The rebuilt holder/builder selection passes **97 tests** in 29.49 seconds
(`/tmp/puyasol-rev-2-holders-slot-bindings.xml`), and all **19 native tests** pass
in 1.22 seconds. New shadowed-local and multi-return `.slot` cases run in all eight
storage/ABI/code-generation profiles and agree with solc 0.8.34/Cancun,
legacy pipeline, optimizer disabled.

The following full sweep recorded **1,846 passed, 2 failed, 101 xfailed and
39 xpassed** in 322.17 seconds (`/tmp/puyasol-rev-2-final-semantic.xml`). Besides
the known DCE failure, the shared `.slot` path had incorrectly applied arithmetic
operand validation to the native string-storage identity sentinel used by
`test_storage_slot_write_through`. Keeping the original slot conversion behavior
alongside the shared binding redirect restores that existing regression. The
final focused selection passes **98 tests** in 27.54 seconds
(`/tmp/puyasol-rev-2-holders-final-focused.xml`); all **19 native tests** pass in
1.21 seconds. No test markers or existing runtime assertions were weakened.

### Checkpoint 12 — final combined verification

The final full run on 2026-09-06 recorded **1,847 passed, 1 failed, 101 xfailed
and 39 xpassed** (1,988 total) in 328.12 seconds. Report:
`/tmp/puyasol-rev-2-final-semantic-v2.xml`. The sole failure is the unchanged
`test_dce_reverting_subexpr_literal_folds`: `divdivShl(uint256)(0)` returns zero
instead of reverting. There are no additional failures; the suite is not fully
green. XPASS/xfail markers were not reclassified.

```bash
PUYASOL_LOCALNET_RESET=0 pytest framework/test_compile_cache.py \
  framework/test_harness.py tests/ -q -n 2 --tb=short \
  --junitxml=/tmp/puyasol-rev-2-final-semantic-v2.xml
```

The compiler stayed fixed throughout the run, with SHA-256
`15f5ab2383f56310a07058bbe78fd99b6d91259afc0aa58466573ea5c9a729f5`.
All 19 native CTests and the 98-case focused selection above used that build.
The complete branch has **471 fewer net lines in `src/`** than its base,
including the new 102-line storage-key helper. Solc/Puya pins, generated artifacts
and LocalNet ledger state were preserved; no dependency update, CI work, migration
or test-marker review was included.

### Baseline and remaining gates

The recorded pre-branch semantic baseline is 1,688 passed, 1 failed, 102 xfailed,
38 xpassed (1,829 total), with 16 native CTests passing; see the
[semantic test guide](../tests/solidity-semantic-tests/README.md). It was measured
at `0d035ea53d`, before the artifact-untracking commit at this branch's base.
Do not present it as a new run on `rev-2`.

Pinned dependencies at branch creation:

- Solidity: `a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d`.
- Puya: `27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1).

Audit evidence was limited to source tracing, compiler/TEAL probes, a native
size-helper probe, an in-process solc/EVM comparison, and five focused CTests.
The attempted local AVM dry-run endpoint returned HTTP 404. That is not AVM
runtime confirmation and does not justify calling these fixes verified.

- [x] Add focused regressions under the existing semantic categories and native
  tests for pure type/layout/size logic. Keep expectations explicit and derived
  from solc; record the oracle compiler, optimization and EVM settings.
- [x] Exercise both storage modes and both ABI profiles where supported. For
  native-only representations outside the EVM domain, assert preservation and
  invariants directly rather than claiming an impossible EVM byte-for-byte match.
- [x] Verify runtime results, not just compilation or TEAL shape; test exact
  deployment schemas and check that read-only default handling adds no state.
- [x] Build and run all native tests after shared-helper changes. Run focused
  semantic groups after each fix and full semantic coverage at storage/refactor
  checkpoints and once more on the final compiler, without rebuilding mid-run.
- [x] Never reset LocalNet. Always use `PUYASOL_LOCALNET_RESET=0`; preserve existing
  artifacts and ledger state. Keep generated reports untracked/outside the repo.
- [x] Report every failure separately. The existing
  `test_dce_reverting_subexpr_literal_folds` failure remains visible; no new xfail
  or weakened assertion may disguise it or a branch regression. A run with that
  failure is not all green, even if this branch adds no regressions.
- [ ] Record tested commits, dependency revisions, commands, counts and report
  paths. Summarize code removed and any representation/layout changes; source
  reduction is desirable but subordinate to correctness.
- [ ] Update this checklist and maintained documentation, commit focused changes
  on `rev-2`, and publish the branch when requested. Do not merge into `main` as
  part of planning. Retire this working plan after completion rather than leave
  an outdated audit backlog.

Standard final checks, from the repository root:

```bash
cmake --build build -j 3
ctest --test-dir build --output-on-failure
git diff --check
```

From `tests/solidity-semantic-tests`, against the final built compiler:

```bash
PUYASOL_LOCALNET_RESET=0 pytest framework/test_compile_cache.py framework/test_harness.py -q
PUYASOL_LOCALNET_RESET=0 pytest tests/ -q -n 2 --tb=short \
  --junitxml=/tmp/puyasol-rev-2-semantic.xml
```

Completion means all F1-F9 and R1-R3 are implemented, their focused tests pass,
compatibility decisions are resolved, and full-suite evidence is recorded with
no new regressions. An unrelated baseline failure must still be reported as a
failure, not folded into a claim that all semantic tests passed.
