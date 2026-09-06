# rev-2: sol-types and storage implementation plan

Status: in progress. F1, F2, F4, F7, F9 and R1 are implemented and checkpoint-test
verified; the remaining findings/refactors and final full-suite verification are pending.

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

- [ ] Replace unseparated root/member-name concatenation in
  [MappingPrefix.cpp](../src/builder/sol-ast/MappingPrefix.cpp) with one structured
  holder-path representation and one unambiguous encoder.
- [ ] Derive logical identity from solc-resolved declarations, root slots and
  member offsets. Encode segment kinds/boundaries explicitly where paths remain
  structured. Compilation-local AST IDs must not become persisted keys.
- [ ] Route direct accesses, storage aliases, internal/library reference
  parameters, nested mappings and getters through that same encoding rule.
  Keep ARC-56 descriptions and test-harness key construction consistent.
- [ ] Test distinct holder paths for isolation and equivalent alias paths for
  identity; preserve existing dynamic mapping-key hashing regressions.
- [ ] Resolve and document the persisted-key compatibility gate below before
  changing the default physical format.

Acceptance: holder-path identity is independent of ambiguous name concatenation,
and every consumer agrees on the physical key. The remaining audit finding is
holder-path framing, not the dynamic string/bytes key hashing already present.
This finding was source-traced, not runtime-confirmed in the audit.

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

- [ ] Remove the assumption in
  [StorageMapper.cpp](../src/builder/storage/StorageMapper.cpp) that every large
  fixed-width box has already been created. Use explicit eager/lazy lifecycle
  information from R3, including after deletion.
- [ ] Handle absent lazy storage at the requested element/window level, returning
  the type-correct zero/default without materializing an oversized stack value.
  Reuse this behavior for explicit reads and generated getters.
- [ ] Keep bounds validation independent of box existence. Reads must not create
  boxes; first partial writes must create a valid backing representation.
- [ ] Test unwritten, written and deleted values; scalar and aggregate elements;
  stack-capacity boundaries; and supported multi-box boundaries. Reject any
  unsupported shape explicitly rather than treating it as initialized.

Acceptance: valid reads from unwritten mapping entries return Solidity defaults
without storage mutation or AVM capacity violations. The audit observed a bare
box extraction with no existence/default handling in generated TEAL.

### F6 — Unify transient scalar representation and packing

- [ ] Consume solc's transient logical layout, rather than independently
  reconstructing inheritance order and packed offsets in
  [TransientStorage.cpp](../src/builder/storage/TransientStorage.cpp).
- [ ] Resolve UDVT underlying types consistently. Distinguish logical Solidity
  width from native physical width for addresses, contract values and function
  pointers using the same representation facts as other storage backends.
- [ ] Reuse [SlotWordCodec](../src/builder/storage/SlotWordCodec.h) for canonical
  word packing/unpacking. Keep any native-width/shadow adaptation explicit;
  blindly applying a 20-byte codec to a native address is not sufficient.
- [ ] Test direct and wrapped values, signed widths, bytes alignment, inheritance,
  neighboring packed values, high-level/Yul agreement, and transient lifetime
  across supported call boundaries in both ABI profiles.

Acceptance: wrapped and unwrapped values round-trip consistently; assembly uses
solc's logical slots/offsets; all supported native address bits survive. The
audit confirmed a direct/wrapped address width mismatch in generated TEAL.
Any unavoidable new non-exact behavior requires a separate policy decision.

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

- [ ] Replace the blanket ABI-predicate bypass with a storage-shape query over
  valid solc facts: dynamic array size, base types and struct member types.
  Reuse safe solc predicates where applicable; keep recursive-type traversal
  guarded and do not invoke ABI encoding queries on non-ABI types.
- [ ] Share the classification between storage placement and reference/handle
  passing in [StorageRefPointer.h](../src/builder/sol-ast/StorageRefPointer.h).
- [ ] Preserve legitimate small fixed aggregates as fixed values. A dynamic
  array's one-slot solc storage head is not a bound on its serialized contents.
- [ ] Test dynamic arrays, nested/fixed containers of dynamic arrays, structs,
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

- [ ] Replace `toString(true)` plus recursive `declarationIdentitySuffix()` in
  [TypeMapper.cpp](../src/builder/sol-types/TypeMapper.cpp) with solc identifiers
  after deliberate representation normalization.
- [ ] Specify which memory/storage/calldata distinctions are intentionally shared
  by the runtime representation and which must remain distinct. Raw identifier
  substitution alone is not a behavior-preserving change.
- [ ] Preserve nominal identity for structs, enums, UDVTs and contracts, including
  nested types and function signatures. Test same-named declarations from distinct
  sources, recursive projections and all relevant data locations.
- [ ] Retain valid recursive construction/caching behavior and test consumers
  that rely on interned WType pointer identity. Never use these compilation-local
  identifiers as a persisted-storage format.

Acceptance: equivalent representations intern consistently, distinct nominal
types do not alias, and the handwritten identity-suffix recursion is removed.

### R3 — Shared storage binding, lifecycle and representation facts

- [ ] Extend the existing physical binding/runtime-plan model instead of adding
  another competing planner. Record declaration identity, storage class,
  logical layout, physical representation, and eager/lazy/default behavior.
- [ ] Have initialization, reads/writes, reference passing, getters and schema
  generation consume the same facts. Runtime mapping entries still carry their
  own keys and existence state; declaration metadata alone cannot prove existence.
- [ ] Replace lifecycle guesses based on raw AWST expression shape where the
  new facts apply. Reuse the canonical word codec for transient operations.
- [ ] Remove redundant name/type arguments, unused name-index maps and duplicate
  default/packing logic once all affected consumers and tests are migrated.

Acceptance: the initial refactor preserves layout and behavior byte-for-byte;
intentional fixes are separate, test-backed changes. Do not remove backend
workarounds merely because they appear repetitive or reference stale documents.

## Persisted-storage compatibility gate

F3 changes physical keys. F8 and any revised placement estimates can move state
between global cells and boxes. Neither is a harmless refactor for an existing
deployment, even when the new behavior agrees better with Solidity.

- [ ] Inventory affected layouts and compare representative pre/post manifests.
- [ ] Propose an explicit format/version boundary and fresh-deployment behavior.
  Prefer stable identities derived from solc logical layout, not source spelling
  or AST numbering. Solc-compatible layout changes still require compatibility
  review when applied to existing contracts.
- [ ] Obtain a decision before switching an existing persisted format: a breaking
  fresh-deployment-only release, or an explicitly versioned compatibility path.
  Do not silently reinterpret old keys or automatically read ambiguous legacy
  locations. Any migration of deployed state is a separate authorized task.
- [ ] Update ARC-56 metadata, related tooling and documentation with the decision.

Unrelated fixes and the format design can proceed while this decision is pending.
F3 is not complete merely because its design is written down.

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

- [ ] Add focused regressions under the existing semantic categories and native
  tests for pure type/layout/size logic. Keep expectations explicit and derived
  from solc; record the oracle compiler, optimization and EVM settings.
- [ ] Exercise both storage modes and both ABI profiles where supported. For
  native-only representations outside the EVM domain, assert preservation and
  invariants directly rather than claiming an impossible EVM byte-for-byte match.
- [ ] Verify runtime results, not just compilation or TEAL shape; test exact
  deployment schemas and check that read-only default handling adds no state.
- [ ] Build and run all native tests after shared-helper changes. Run focused
  semantic groups after each fix and full semantic coverage at storage/refactor
  checkpoints and once more on the final compiler, without rebuilding mid-run.
- [ ] Never reset LocalNet. Always use `PUYASOL_LOCALNET_RESET=0`; preserve existing
  artifacts and ledger state. Keep generated reports untracked/outside the repo.
- [ ] Report every failure separately. The existing
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
