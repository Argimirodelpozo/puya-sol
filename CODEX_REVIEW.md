# Builder review

This review was performed on `codex-review`, with emphasis on `src/builder/`
and with the active semantic pytest suite used as the behavioral check.

## Changes made

- Replaced `StorageLayout::SlotInfo` pointers into a growing vector with stable
  variable indices. The previous representation could dereference invalidated
  pointers after `m_variables` reallocated.
- Added a compilation-owned session containing target options, whole-program
  analysis, source mapping, generated artifacts, synthetic type ownership, and
  function-pointer dispatch state. This removes reset ordering and cross-build
  leakage from the corresponding process globals.
- Made all dynamically synthesized `WType` objects owned by `TypeMapper`; no
  `new awst::...` allocations remain in the builder.
- Replaced callback members on `ContractContext` with ordinary methods and made
  operand lowering return a value plus explicit pre/post effects.
- Added source-level `CallPlan` and lowered-target `LValuePlan` classifications
  so call and assignment dispatch have one decision point rather than repeated
  syntax probing.
- Centralized whole-program AST prepasses in `ProgramAnalysis` and contract
  storage facts in `StorageRuntimePlan`.
- Split EVM storage lowering into address/path resolution and value read/write
  implementation files.
- Added whole-value EVM-layout support for dynamic arrays whose elements are
  one-slot fixed boolean arrays. The runtime codec now translates Solidity's
  byte-per-bool slot lanes to ARC4's MSB-first bit packing in both directions.
- Removed remaining mutable generated-name counters and the global child-
  contract/template registry from builder code.

## solc facilities to prefer

- EVM-mode `StorageLayout` now consumes solc's
  `ContractType::linearizedStateVariables(DataLocation::Storage)` directly.
  This is safer than reproducing inheritance, packing, `layout at`, immutable,
  constant, and transient exclusion rules locally.
- `CallPlan` relies on solc's resolved `FunctionType`, referenced declaration,
  call kind, and contract annotations. New call special cases should extend
  that plan rather than infer semantics from identifier/member spelling.
- Existing `SolcConstFold` should remain the only entry point for compile-time
  Solidity expression folding. Assembly-only Yul folding still needs its own
  path because it operates on a different AST.

Default-mode storage still intentionally uses its legacy layout walk because
its named-cell backend has behavior coupled to that representation; switching
it wholesale to solc's EVM layout caused broad default-mode regressions in the
existing suite. A future replacement should first separate named-cell identity
from EVM slot placement, then use solc metadata for both modes.

## Useful follow-ups

- `StorageDispatch.cpp` remains large even after extracting runtime analysis.
  Its generated methods can next be split into dense-page, sparse-slot,
  bytes/string, and dynamic-array emitters.
- The explicit operand result currently adapts to legacy flat pending buffers.
  Migrating every expression builder to return effects directly would make
  evaluation order structural and allow those buffers to disappear.
- `FunctionTranslationCtx` and `sol_ast::FunctionContext` still mirror some
  fields. One function-scoped owner with small views for block/modifier
  translation would remove synchronization risk.
- Scratch layout deserves its own allocator. Configuring more than five EVM
  memory slots can overlap the fixed transient and AVM flash-accounting scratch
  ranges; the pre-existing option assumes those features are not combined.

## Additional design review — 2026-08-15

### Highest priority

1. **Never lower an unsuccessfully analyzed solc AST.** `parseAndAnalyze()` can
   currently fail while the driver continues if the remaining diagnostics match
   compatibility-error strings. Builder relies on completed solc annotations
   (`type`, `referencedDeclaration`, call kind, call graphs, and virtual lookup),
   none of which are guaranteed after failed analysis. Compatibility rewriting
   must produce a source tree that solc accepts, or compilation must stop. This
   preserves solc as the single semantic validator and avoids a redundant AWST
   prevalidation pass.

   The legacy duplicate-event rewrite is itself unsafe: it matches only event
   names, triggers when the source contains any inheritance, scans only direct
   relative imports, and does not establish that the imported interface is an
   actual base. It can delete an unrelated overload. It should match complete
   signatures and real inheritance, or legacy sources should be normalized with
   a matching legacy solc frontend.

2. **Make expression effects structural.** `pendingStatements` and
   `prePendingStatements` remain an implicit side channel across expression and
   statement builders. The statement-boundary salvage warning documents that
   effects can still leak into a later statement or function. Make every
   expression return a `LoweredExpression { value, before, after, solType }` and
   have parents compose those effects explicitly. This should ultimately remove
   the flat buffers, snapshot/tail extraction, leak salvage,
   `pendingArrayPushValue`, and much of the conditional-depth/effect-scan logic.

3. **Use solc's Yul analysis utilities.** `AssemblyBuilder` manually discovers
   nested functions, constructs a direct call graph, computes reachability and
   recursion, and walks assignments. Vendored solc already provides
   `allFunctionDefinitions`, `assignedVariableNames`,
   `CallGraphGenerator::callGraph`, and `CallGraph::recursiveFunctions`.
   `ProgramAnalysis` should likewise use the inline assembly's actual dialect,
   the dialect's storage load/store handles, and solc's side-effect propagation
   instead of constructing a hard-coded Cancun dialect and scanning opcodes.

### Direct solc and identity improvements

4. **Use solc's canonical storage layout in every mode.** Canonical EVM mode
   already consumes `linearizedStateVariables`; default-mode assembly dispatch
   still repeats Solidity's packing algorithm and de-duplicates inherited state
   variables by name. Separate logical Solidity placement (always solc's
   declaration-ID to slot/offset mapping) from physical AVM cell binding. The
   default dispatcher can compose canonical slot words from named physical
   cells. This removes the hand-written packing walk and its differential
   tripwire.

5. **Use declaration identity for internal functions.** Internal subroutine IDs
   are currently assembled from names, parameter counts, sequence numbers, and
   collision suffixes even though resolved calls already carry a globally unique
   solc declaration ID. A `FunctionSymbolTable` keyed by declaration ID should
   own opaque internal IDs; readable names should be metadata only. This can
   remove most overload-name maps and fallback collision logic.

6. **Make parameter-mutation summaries contract-aware.** The current analysis
   follows the declared virtual target, so an override that mutates more
   reference parameters can lose a required caller write-back. Summaries should
   be keyed by `(mostDerivedContractId, functionId)`, use
   `FunctionDefinition::resolveVirtual`, use solc's `willBeWrittenTo` annotation
   and `FunctionType::Kind::ArrayPush/ArrayPop`, and solve recursive call-graph
   components to a fixed point.

### Solidity semantics versus AVM transport

7. **Separate Solidity selectors from ARC-4 routing selectors.** Solidity-visible
   function/event selectors and interface IDs currently use ARC-4 identities
   because the AVM router uses them. solc already exposes `externalSignature`,
   `externalIdentifier`, and keccak interface selectors. Solidity expressions
   should retain Solidity/EVM values while an explicit transport layer maps them
   to ARC-4 routing. This is migration-sensitive but improves replay fidelity and
   removes manual signature fallbacks.

8. **Centralize unsupported-feature policy.** Some unsupported EVM values still
   compile to plausible constants (`creationCode`/`runtimeCode` as zero bytes,
   fixed `chainid`, sentinel gas limit, placeholder coinbase, and some successful
   call results). Every feature should be classified as exact, documented AVM
   adaptation, configurable environment input, or a hard compile error. solc's
   `CompilerStack::object()` and `runtimeObject()` can provide actual EVM bytecode
   for `type(C).creationCode`/`runtimeCode` hashing and constant comparisons;
   unresolved links should fail explicitly.

### Larger experiment

9. **Prototype a solc-IR backend.** `CompilerStack` exposes ordinary and optimized
   Yul IR. A second backend that lowers solc-generated Yul to AWST could let solc
   own evaluation order, modifiers, inheritance, ABI coding, checked arithmetic,
   conversions, storage addressing, getter generation, and optimization. Start
   as a differential backend for full EVM layout; generated Yul is broader than
   user inline assembly, so this should not begin as an all-at-once rewrite.

### Smaller design improvements

- Replace `AssemblyBuilder`'s process-global mutable scratch configuration with
  an immutable, compilation-session-owned `ScratchLayout` that allocates and
  validates memory, transient, and flash-accounting ranges centrally.
- Merge `FunctionTranslationCtx` and `sol_ast::FunctionContext` into one owned
  per-function state with lightweight block/modifier views, eliminating mirrored
  flags, parameter metadata, return-wire plans, and pointer lifetime coupling.
- Carry the source solc `Type const*` alongside every lowered expression. Build a
  semantic `ConversionPlan(sourceSolType, targetSolType, context)` and keep AWST
  representation emission separate instead of reconstructing Solidity intent
  from `WType` after lowering.
- Put direct use of solc internal APIs behind a small `SolcFacts` facade. This
  keeps the builder concise while localizing coupling to the vendored solc
  version.

### Selected implementation status — 2026-08-15

Items 1, 2, 3, and 5 above are implemented on `codex-new-review-stuff`, along
with the four smaller design improvements. The compatibility pass now either
produces a solc-valid source tree or compilation stops; expression effects are
scoped structural results; Yul facts come through `SolcFacts`; and internal,
library, and free-function symbols use resolved solc declaration identity.

The smaller changes add a session-owned `ScratchLayout`, consolidate the
function translation state into `sol_ast::FunctionContext`, preserve source
solc types in lowered expressions and use `ConversionPlan` at the first three
implicit-conversion sites, and isolate the selected solc Yul APIs behind the
facade. Items 4, 6, 7, 8, and 9 remain proposals only.
