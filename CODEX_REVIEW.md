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
- Made failed solc analysis terminal and constrained legacy event compatibility
  rewriting to exact declarations on directly named imported interfaces.
- Replaced context-wide expression-effect buffers with structurally scoped
  `LoweredExpression` results and statement-owned effect frames.
- Replaced the hand-written Yul function/call/assignment/storage walkers with a
  `SolcFacts` facade over the vendored solc analysis utilities.
- Replaced name-derived internal/free/library function identities with opaque
  symbols keyed by resolved solc declaration ID.
- Added a compilation-owned `ScratchLayout`, merged the duplicate function
  translation contexts, and introduced source-type-aware `ConversionPlan`s.

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
- Expression effects are now statement-scoped structural results. Individual
  legacy helpers still append through `EffectBuffer` compatibility views;
  converting those helpers to return effects directly would remove the final
  adapter and local snapshot/tail logic.
- `ConversionPlan` now covers plain assignment, variable initialization, and
  internal-call arguments. Return/ABI-boundary representation paths can migrate
  incrementally where they express a Solidity implicit conversion rather than a
  transport encoding.

## Additional design review — 2026-08-15

### Highest priority

1. **Done — never lower an unsuccessfully analyzed solc AST.** The driver now
   stops on every `parseAndAnalyze()` failure. Builder relies on completed solc
   annotations (`type`, `referencedDeclaration`, call kind, call graphs, and
   virtual lookup), none of which are guaranteed after failed analysis.
   Compatibility rewriting must produce a source tree that solc accepts, or
   compilation stops. This preserves solc as the single semantic validator and
   avoids a redundant AWST prevalidation pass.

   The legacy duplicate-event rewrite now uses token-normalized exact
   declarations and only removes a declaration from a contract/interface that
   directly names the imported interface as a base. Qualified, aliased,
   indirect, or otherwise unresolved shapes remain untouched and fail safely in
   solc rather than deleting source speculatively.

2. **Done at the structural boundary — make expression effects structural.**
   Every primary expression lowering now returns a
   `LoweredExpression { value, effects, solType }`. Nested expressions and each
   Solidity statement receive their own effect frame, so undrained work cannot
   leak into the next statement or function. The statement-boundary salvage was
   removed, and `pendingArrayPushValue` became an RAII-scoped assignment value.
   `EffectBuffer` remains as a frame-backed compatibility view for legacy
   emitters; removing that adapter is cleanup rather than a correctness gap.

3. **Done — use solc's Yul analysis utilities.** `AssemblyBuilder` now obtains
   nested functions, assignments, call-graph reachability, and recursion through
   vendored solc's
   `allFunctionDefinitions`, `assignedVariableNames`,
   `CallGraphGenerator::callGraph`, and `CallGraph::recursiveFunctions`.
   `ProgramAnalysis` uses the inline assembly's actual dialect and solc's
   side-effect propagation instead of constructing a hard-coded Cancun dialect
   and scanning opcode spellings.

### Direct solc and identity improvements

4. **Outstanding — use solc's canonical storage layout in every mode.**
   Canonical EVM mode already consumes `linearizedStateVariables`; default-mode
   assembly dispatch still repeats Solidity's packing algorithm and
   de-duplicates inherited state variables by name. Separate logical Solidity
   placement (always solc's declaration-ID to slot/offset mapping) from physical
   AVM cell binding. The default dispatcher can compose canonical slot words
   from named physical cells. This removes the hand-written packing walk and
   its differential tripwire.

5. **Done — use declaration identity for internal functions.** A
   `FunctionSymbolTable` now assigns opaque AWST symbols from globally unique
   solc declaration IDs for internal/private contract methods and free/library
   subroutines. Calls and function-pointer targets use solc's resolved virtual
   declaration, preserving most-derived dispatch without source-name collision
   machinery. Public ARC-4 route names remain readable transport identities.

6. **Done — parameter-mutation summaries are contract-aware.**
   `ParameterMutationAnalysis` keys completed summaries by
   `(mostDerivedContractId, exactFunctionBodyId)`, resolves Virtual, Static, and
   Super call sites with solc's lookup annotations and `resolveVirtual`, records
   direct lvalues through solc's `willBeWrittenTo`, recognizes array mutation by
   `FunctionType::Kind::ArrayPush/ArrayPop`, and propagates parameter-position
   effects to a least fixed point across recursive call components. Callee
   return augmentation, caller write-back, and the mutable-alias guard now
   consume the same summary instead of rescanning bodies independently.

   Modifier memory-reference arguments remain a separate lowering limitation:
   the modifier chain currently copies them into modifier locals and does not
   thread those locals back. Folding modifier effects into these summaries
   before fixing that transport would only add a return slot containing the
   unchanged outer value, so this analysis intentionally summarizes function
   bodies until modifier arguments become true aliases.

### Solidity semantics versus AVM transport

7. **Outstanding — separate Solidity selectors from ARC-4 routing selectors.**
   Solidity-visible function/event selectors and interface IDs currently use
   ARC-4 identities because the AVM router uses them. solc already exposes
   `externalSignature`, `externalIdentifier`, and keccak interface selectors.
   Solidity expressions should retain Solidity/EVM values while an explicit
   transport layer maps them to ARC-4 routing. This is migration-sensitive but
   improves replay fidelity and removes manual signature fallbacks.

8. **Outstanding — centralize unsupported-feature policy.** Some unsupported
   EVM values still compile to plausible constants
   (`creationCode`/`runtimeCode` as zero bytes, fixed `chainid`, sentinel gas
   limit, placeholder coinbase, and some successful call results). Every feature
   should be classified as exact, documented AVM adaptation, configurable
   environment input, or a hard compile error. solc's `CompilerStack::object()`
   and `runtimeObject()` can provide actual EVM bytecode for
   `type(C).creationCode`/`runtimeCode` hashing and constant comparisons;
   unresolved links should fail explicitly.

### Larger experiment

9. **Outstanding — prototype a solc-IR backend.** `CompilerStack` exposes
   ordinary and optimized Yul IR. A second backend that lowers solc-generated
   Yul to AWST could let solc own evaluation order, modifiers, inheritance, ABI
   coding, checked arithmetic, conversions, storage addressing, getter
   generation, and optimization. Start as a differential backend for full EVM
   layout; generated Yul is broader than user inline assembly, so this should
   not begin as an all-at-once rewrite.

### Smaller design improvements

- [x] Replace `AssemblyBuilder`'s process-global mutable scratch configuration with
  an immutable, compilation-session-owned `ScratchLayout` that allocates and
  validates memory, transient, and flash-accounting ranges centrally.
- [x] Merge `FunctionTranslationCtx` and `sol_ast::FunctionContext` into one owned
  per-function state with lightweight block/modifier views, eliminating mirrored
  flags, parameter metadata, return-wire plans, and pointer lifetime coupling.
- [x] Carry the source solc `Type const*` alongside every lowered expression. Build a
  semantic `ConversionPlan(sourceSolType, targetSolType, context)` and keep AWST
  representation emission separate instead of reconstructing Solidity intent
  from `WType` after lowering. The plan is active for assignment,
  initialization, and internal-call arguments; other representation-only
  coercions intentionally remain outside it.
- [x] Put direct use of solc internal APIs behind a small `SolcFacts` facade. This
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
