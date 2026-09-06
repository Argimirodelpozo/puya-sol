# rev-2: sol-types and storage results

Completed on 2026-09-06. All F1–F9 and R1–R3 are implemented on `rev-2`, based
on `3b9a82d8e46c4ca99873b4fb1d7239d1ed50771b`. The tested compiler and regression
sources are committed as `45ecf7139c024569e94bc36e008d70db9c6e36a0`.

The full suite has **no new failures**, but is **not fully green**: the existing
Puya DCE divide-by-zero bug remains a normal failing test. This completed record
replaces the working plan; the original acceptance criteria and detailed
intermediate checkpoints remain in `docs/rev-2-plan.md` at the tested commit.

## Completed work

| Finding | Result |
|---|---|
| F1 + R1 — aggregate conversions | One conversion route selects from solc types before emitting effects. Scalar widening is shared across array shapes; effectful sources are evaluated once. Duplicate assignment/initializer/argument/return paths were removed. |
| F2 — assembly constants | Typed solc literals and `ConstantEvaluator` replace spelling-based classification; fixed-bytes alignment and padding survive constant chains. |
| F3 — mapping-holder identity | One versioned, framed encoder consumes full solc root slots, member offsets and checked array indices. Direct access, aliases, references, getters, ARC-56 metadata and tooling agree. Runtime bindings survive tuple swaps, shadowing and Yul assignments. |
| F4 — getter bounds | Full-width indices are validated before narrowing, including arrays around mapping levels and nested getter paths. |
| F5 — absent large values | Bounded projections return Solidity defaults without creating boxes. Partial writes create valid backing values; deletion and paged-array reuse share checked lifecycle handling. |
| F6 — transient words | Solc supplies inheritance order and packed offsets. Typed access shares `SlotWordCodec`, with explicit preservation of native-only address bytes. |
| F7 — transient initialization | Transient declarations are excluded from persistent schema and constructor initialization; exact-schema deployments are tested. |
| F8 — dynamic callback aggregates | Storage shape follows solc array/member facts without invoking unsupported ABI predicates. Dynamic callback aggregates use boxes; small fixed controls remain global. |
| F9 — checked sizes | Fixed, dynamic, unsupported and overflow outcomes are distinct. Target narrowing/allocation is checked while full-width logical slots and compile-time array facts remain available. |
| R2 — type identity | Canonical solc identifiers are used after explicit representation normalization. Nominal identity, recursive projections, callable locations, calldata slices and cache resets are covered. |
| R3 — shared storage facts | Contract-scoped bindings, logical placement, physical representation and eager/lazy lifecycle facts feed initialization, access, references, getters and schema. Duplicate codecs and unused name/type paths were removed. |

The complete branch removes **471 net lines from `src/`**: 1,860 additions and
2,331 deletions relative to its base, including the new storage-key helper.
This is source reduction, not a claim about emitted TEAL or binary size.

Implementation commits: `032b8399da`, `eefde7721b`, `aa4ab5031a`, `b9fe763671`,
`c07ebf7f27`, `bd1956b410`, `b3f596cc11`, `6f3b0fbbc5`, `2f122384fc` and
`45ecf7139c`. Documentation checkpoints are separate commits.

## Authority and compatibility

The pinned [solc types API](../solidity/libsolidity/ast/Types.h) and
[implementation](../solidity/libsolidity/ast/Types.cpp) are authoritative for
type identity, conversions, logical placement, member offsets and array facts.
ARC4 sizes and AVM capacities remain target-specific representation decisions.

The user approved the following boundaries on 2026-09-06:

- **F3/F8 are fresh-deployment-only changes.** Existing applications must keep
  their original compiler and artifacts. There is no legacy-key fallback,
  reinterpretation of old state or migration.
- Default mapping-containing roots and their descendant mapping boxes use
  [holder format 2](storage-format.md). Names remain artifact labels, not key
  inputs. ARC-56 describes exact roots, not fictitious hash-entry prefix maps.
- Nonrecursive single-struct wrappers at solc coordinate `(0, 0)` with the same
  storage extent are transparent. They retain nominal identity but add neither
  a holder step nor an ARC4 wrapper header. Their persisted payload changes too.
- Dynamic aggregates containing internal functions move from named globals to
  boxes where required. Small fixed controls and ordinary mapping-free keys
  remain unchanged. EVM slot-mode persistent keys are unchanged.
- **F6 preserves full native addresses.** High address bytes live in private
  scratch outside the canonical transient word. Raw `tstore` clears native-only
  bytes for declarations in that word. See the
  [approved transient adaptation](../EVM_DIVERGENCE.md#transient-words-and-native-addresses).

Unsupported reference/capacity shapes diagnose explicitly. In default layout,
key-only aggregate parameters/returns containing mappings must identify a whole
box, including a mapping-entry box; transparent wrappers preserve that place.
General interior aggregate handles require a richer representation. Direct
nested updates, local aliases, mapping-field references and whole-enclosing-box
references remain supported. Interior dynamic-array resize references, oversized
mapping-value paging, and existing slot-mode whole-array limits are not silently
lowered to guessed storage locations.

Staticcall remains accepted and warning-only. No CI changes, dependency updates,
XPASS/xfail reclassification, unrelated payment changes or ledger migration were
included.

## Final verification

| Selection | Result | Duration |
|---|---|---:|
| Full semantic + harness/cache suite | 1,847 passed; 1 failed; 101 xfailed; 39 xpassed — 1,988 total | 328.12 s |
| Focused holder/builder regressions, including EnumerableSet, mapping tuples and StorageSlot write-through | 98 passed | 27.54 s |
| Native CTest suite | 19/19 passed | 1.21 s |

The sole full-suite failure is
`puyasolRegression/test_puyasol_regression.py::test_dce_reverting_subexpr_literal_folds`:
`divdivShl(uint256)(0)` returns zero instead of reverting. It is an open backend
bug, **not** an accepted divergence. No assertion was weakened or new xfail
added to conceal it. Xpasses are non-strict and are not counted as ordinary
passes; their review remains deferred.

Pinned dependencies:

- Solidity: `a99b6d8c0cbf9eddbac104e8e4e16545db7d3d8d`.
- Puya: `27751c364229ae3cd0334fe4071e61690b6879e4` (5.10.1).

The compiler was not rebuilt or replaced during any full run. The final binary
SHA-256 is `15f5ab2383f56310a07058bbe78fd99b6d91259afc0aa58466573ea5c9a729f5`.
An incremental build after committing reproduced the same binary hash and
recorded a clean source tree at `45ecf7139c` in the build manifest.

Commands used, from the repository root:

```bash
CCACHE_DISABLE=1 cmake --build build -j 3
ctest --test-dir build --output-on-failure -j 3
git diff --check
```

From `tests/solidity-semantic-tests`:

```bash
PUYASOL_LOCALNET_RESET=0 pytest framework/test_compile_cache.py \
  framework/test_harness.py tests/ -q -n 2 --tb=short \
  --junitxml=/tmp/puyasol-rev-2-final-semantic-v2.xml

PUYASOL_LOCALNET_RESET=0 pytest \
  tests/puyasolRegression/test_storage_holders.py \
  tests/puyasolRegression/test_builder_findings.py \
  tests/puyasolRegression/test_puyasol_regression.py::test_enumerable_set_values \
  tests/puyasolRegression/test_puyasol_regression.py::test_storage_slot_write_through \
  tests/variables/test_variables.py::test_mapping_local_tuple_assignment \
  -q -n 2 --tb=short --junitxml=/tmp/puyasol-rev-2-holders-final-focused.xml
```

New runtime expectations were checked with an in-process solc 0.8.34/Cancun
oracle, legacy pipeline and optimizer disabled. ERC-7201 builtins instead use
the pinned development compiler's facts and existing expectations; 0.8.34 does
not expose that builtin. Tests exercise both ABI, storage and code-generation
profiles where supported, exact schemas, default-read non-mutation, isolation,
bounds, alias lifetime and deletion/reuse. Native-only address preservation is
asserted directly on AVM, not claimed as an EVM byte-for-byte comparison.

## Retained evidence

LocalNet was **not reset**. Existing ledger state, generated artifacts and
archives were preserved. JUnit reports remain outside the repository; generated
compiler/test outputs remain ignored.

Representative before/after artifacts are under
`/tmp/puyasol-rev-2-format-before.eL6NrY/`:

- `functions` / `functions-after`: four dynamic callback aggregates change
  placement; two small fixed controls remain global.
- `holders-format-fixture` / `holders-format-after-second`: 14 mapping-containing
  roots change keys; five mapping roots switch to exact-root placeholder metadata.
- `wrappers-format-before` / `wrappers-format-final`: wrapper payload flattening
  is explicit, including array elements. Runtime box reads independently check
  solc-derived root/member keys with transparent steps omitted.

The earlier behavior-preserving binding refactor's complete AWST comparisons
are under `/tmp/puyasol-rev-2-bindings-before` and
`/tmp/puyasol-rev-2-bindings-after`. Intermediate failures, their fixes and report
paths are preserved in the retired plan at `45ecf7139c`; they are not an open
audit backlog. See the [semantic test guide](../tests/solidity-semantic-tests/README.md)
for the current baseline and running instructions.
