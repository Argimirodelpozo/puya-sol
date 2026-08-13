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
