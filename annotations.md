# Solc AST Annotations — Inventory & Adoption Plan

A reading list of every annotation that solc populates during analysis, what
puya-sol currently does with it, and what we could change. Organized by:

- **Already adopted** — annotations puya-sol uses today, with the commit
  that wired them up.
- **Worth adopting** — concrete refactors that delete hand-rolled puya-sol
  logic and use the solc-resolved metadata directly.
- **Caveats** — a couple of annotations that look attractive but only get
  populated under solc compilation paths puya-sol doesn't trigger.

## Already adopted

| Annotation | Where it lives | Where puya-sol reads it |
|---|---|---|
| `Expression::annotation().referencedDeclaration` | every `Identifier`, `MemberAccess`, `IdentifierPath` | universal — every name resolution path. Plus `ASTNode::referencedDeclaration(expr)` static helper recently adopted in SolEmit, SolRevert, AvmLibCallChecker, CtorCallChecker, AsaIntrinsics, SolVariableDeclaration. |
| `Expression::annotation().type` | every Expression | universal — every type-driven decision in SolExpression / SolStatement. |
| `Declaration::annotation().contract` | every FunctionDefinition / VariableDeclaration | adopted in commit `7855005d6` — replaced 7 `decl->scope() + dynamic_cast<ContractDef>` sites. |
| `ContractDefinition::annotation().linearizedBaseContracts` | every Contract | 16 walks across the codebase (storage layout, MRO, super resolution, getter inheritance, etc). |
| `ContractDefinition::annotation().baseConstructorArguments` | every Contract | adopted in `fc8c6bd12` — replaced ~70 LOC of manual InheritanceSpecifier / ModifierInvocation walks. |
| `FunctionDefinition::annotation().baseFunctions` | every override | 1 site (overload-detection in `ContractBuilder.cpp:183`). |
| `ContractDefinition::annotation().baseSlot` | contracts with `layout at N` | 1 site (`StorageLayout.cpp`). |
| `BinaryOperation::annotation().commonType` | every BinaryOp | adopted for binary-op result widening in `SolBinaryOperation.cpp`. |
| `OperationAnnotation::userDefinedFunction` | every `BinaryOp` / `UnaryOp` | binary operator overloading dispatch in `SolBinaryOperation.cpp`. (Unary side currently doesn't check this — gap.) |
| `IndexAccess::annotation().willBeWrittenTo` | every IndexAccess | lvalue routing in `SolIndexAccessHandlers.cpp`. |
| `MemberAccess::annotation().requiredLookup` | every MemberAccess | super-call vs. base-call vs. virtual classification in `SuperCallResolution.cpp`. |
| `FunctionCall::annotation().kind` | every FunctionCall | `FunctionCallKind` dispatch in `SolExpressionFactory.cpp`. |
| `ContractDefinition::storageLayoutSpecifier()` | contracts with explicit base slot | adopted via `baseSlot` (above). |
| `ConstantEvaluator::tryEvaluate` | static helper | adopted in `ba849498a` — replaced `resolveConstantU256` hand-rolled recursion. (Polarity bug on `isConstantVariableRecursive` gate fixed in `933614134`.) |
| `resolveOuterUnaryTuples` | static helper | adopted in `9125edd04` — replaced 2 manual TupleExpression unwraps. |
| `IntegerType::max()` | per IntegerType | adopted in `999711f0e` — replaced manual signed/unsigned max calculation + the dead `ExpressionUtils.h` lookup table. |
| `ASTNode::filteredNodes<T>` | template helper | adopted in `eb22d0ad4` and `7d71f496a` — replaced per-node `dynamic_cast` loops in five sites. |

## Worth adopting

Three buckets, biggest wins first:

### 1. `ContractDefinition::annotation().internalFunctionIDs` — fn-ptr ID allocation

Solc maintains `std::map<FunctionDefinition const*, uint64_t>` per contract,
mapping each function used as an internal-fn-pointer target to a unique
uint64 dispatch ID. Puya-sol re-implements this in
`FunctionPointerBuilder.cpp` via a process-global `s_targets` map keyed by
`(funcDef->id(), awstName)` and a monotonic `s_nextId` counter.

Current state: ~50 LOC of hand-rolled ID allocation + a pre-translation
register/finalize handshake (`registerTarget` → `setSubroutineIds`).

Win: delete `s_targets`, `s_nextId`, the `FuncPtrEntry` struct, and the
two-phase registration. Replace each fn-ptr-ID lookup with
`mostDerivedContract->annotation().internalFunctionIDs.at(funcDef)`.

**Caveat:** see "Caveats" below — solc populates this annotation only when
generating IR (`CompilerStack::annotateInternalFunctionIDs()` runs as part
of the `pipelineConfig` path). Puya-sol doesn't trigger IR generation, so
the annotation may be empty when we read it. Verify with a quick
`compilerStack.contract(name).annotation().internalFunctionIDs.size()`
probe before committing to this refactor.

### 2. `Identifier::annotation().overloadedDeclarations` — name-by-arity scan

`InnerCallHandlers.cpp:350-360` looks up the target of an
`abi.encodeWithSelector` call by walking every base contract's
`definedFunctions()` and matching on `func->name() + arg-count`:

```cpp
for (auto const* base : _ctx.currentContract->annotation().linearizedBaseContracts)
    for (auto const* func : base->definedFunctions())
        if (func->isImplemented() && func->name() == fnName
            && func->parameters().size() == nArgs)
        { target = func; goto foundEwSTarget; }
```

Solc already maintains `overloadedDeclarations` on every Identifier — the
list of all candidate Declarations the name resolves to. We can index by
arity and skip the linearization walk entirely.

Win: ~15 LOC saved + correctness improvement (the current loop is naive
about override resolution).

### 3. `ContractDefinition::annotation().creationCallGraph` / `deployedCallGraph` — pre-translation pruning

Solc populates two `CallGraph` objects per contract during analysis. Each
is a graph of CallableDeclaration → set-of-callees with special nodes for
Entry / InternalDispatch / Constructor.

Puya-sol currently does the equivalent *post-translation* in
`SubroutineReachability.cpp` via a custom `AwstWalker` that walks every
emitted Subroutine's body looking for `SubroutineCallExpression`s. The
unreachable Subroutines are then dropped from the AWST. This works, but:

- It runs **after** translation, so we still pay the AWST construction
  cost for dead code.
- The walker is conservative (only looks at static dispatch).

If we use solc's pre-built call graphs:
- Filter `definedFunctions()` to "reachable from creation- or deploy-time
  entry" *before* the translation loop in `ContractBuilder.cpp:300-373`.
- Skip `buildFunction` entirely for unreachable methods.

Win: faster compile (skip dead AWST construction), simpler reachability
code, more accurate pruning under solc's call-graph semantics.

**Caveat:** same as `internalFunctionIDs` — solc populates these in the
`createAndAssignCallGraphs()` step which runs during `analyze()` (line
624 of `CompilerStack.cpp`). This path *is* triggered by puya-sol's
analysis flow, so the call graphs should be available — unlike
`internalFunctionIDs` which runs later in IR-gen.

### 4. `Expression::annotation().isPure` / `.isConstant`

Marked on every Expression. Tells you whether the expression has no
side-effects (`isPure`) or is fully constant (`isConstant`).

Where it'd help:
- Several `tryConstantFold` paths in SolBinaryOperation, SolUnaryOperation
  hand-roll "is this expression evaluable at compile time" via
  per-AST-node dynamic_cast cascades. They could short-circuit with
  `expr.annotation().isConstant`.
- `assert()` / `require()` argument lifting decisions in
  `SolRequireAssert.cpp` could check `isPure` to decide whether to evaluate
  the failure-message expression for side effects.

Win: small (~10–20 LOC), but trims away duplicated "is this constant"
logic.

### 5. `Expression::annotation().isLValue`

Whether an expression is assignable. Currently we infer this from context
(`dynamic_cast<Identifier> + isStateVariable`). The annotation tells us
directly.

Where it'd help: `SolAssignment.cpp` and `SolAssignmentHandlers.cpp` lvalue
routing — currently re-classifies the LHS via dynamic_cast cascades.
~10 LOC saved.

### 6. `IdentifierPath::annotation().pathDeclarations`

Solc maintains, for every `A.B.C` path, the resolved Declaration at *each*
segment (not just the final one). Puya-sol walks `path()` segments
manually when resolving qualified library calls (`Lib.Sub.f`).

Where it'd help: cross-library member access in `SolInternalCall.cpp` and
`CallResolver.cpp`. Probably ~10 LOC; mostly removes manual segment
re-resolution.

### 7. `FunctionCall::annotation().arguments`

Solc stores the *typed* argument list (`std::optional<FuncCallArguments>`)
that includes solc's chosen implicit conversions. Useful when puya-sol
wants the post-coerced types rather than the raw `arguments()` types.

Where it'd help: `SolStructConstruction.cpp` named-arg handling and
`SolExternalCall.cpp` argument encoding currently re-derive the target
parameter types from the function signature; this annotation gives them
directly.

Win: small per site, but if every call-site adopts it, cumulative ~20 LOC.

### 8. `Block::annotation().hasMemoryEffects` / `.markedMemorySafe`

Solc tracks whether an inline-assembly block touches the EVM memory area.
Puya-sol currently assumes every assembly block can mutate memory and
emits the conservative free-memory-pointer simulation around each.

Where it'd help: `SolInlineAssembly.cpp` could skip the memory-flush
emission for `assembly memory-safe { ... }` blocks. Many production
contracts (OpenZeppelin, Solmate) use this hint — savings could be
non-trivial in TEAL size.

### 9. `Block::annotation().isSimpleCounterLoop`

Marks for-loops with the canonical `for (uint i = 0; i < N; ++i)` shape.
Could enable an induction-variable-based optimization, but probably not
worth it on AVM (we don't have register pressure to optimize for).

Skip unless we hit a perf hot path.

### 10. `StructDefinition::annotation().recursive` / `.containsNestedMapping`

Solc detects recursive structs and structs containing mappings during
analysis. Currently `TypeMapper::mapStruct` has its own
`m_inProgressStructs` recursion guard. Could simplify by checking the
annotation up front.

Win: small (~10 LOC), and gives a clearer error path for structurally
invalid types.

### 11. `Identifier::annotation().candidateDeclarations`

Populated when overload resolution *fails*. Useful for richer error
messages on unresolved names. Currently we just emit a generic warning.
Pure ergonomics improvement, not a refactor.

### 12. `Literal::passesAddressChecksum()` / `.looksLikeAddress()`

Per-literal helpers. Could power a "you wrote `0x123...` which looks like
an address but doesn't pass the checksum" warning. We don't currently warn
on this. Pure ergonomics.

### 13. `MagicVariableDeclaration` typed lookup for `msg`/`block`/`tx`

`SolNewExpression.cpp` and `ApprovalProgramBuilder.cpp`'s `MsgRefChecker`
currently identify references to `msg.value` etc by string-comparing the
identifier name (`id->name() == "msg"`). This breaks if a user-defined
local shadows `msg`. Solc tags the magic globals as
`MagicVariableDeclaration`; checking
`dynamic_cast<MagicVariableDeclaration>(id->annotation().referencedDeclaration)`
is more robust.

Win: tiny code, but plugs a real shadowing bug. Worth doing.

### 14. `FunctionDefinition::annotation().contract` (already adopted) — extend to ModifierDefinition

Same `annotation().contract` pattern works for ModifierDefinition.
Currently `ModifierInliner.cpp:557-580` walks linearization to find the
most-derived modifier override. Could potentially use
`ModifierDefinition::resolveVirtual(mostDerivedContract)` (solc helper)
for the same MRO-aware resolution we get for FunctionDefinition.

Win: ~10 LOC. Caveat: `resolveVirtual` for modifiers asserts
`_searchStart == nullptr`, so it can't handle explicit base-modifier calls.
Need to gate as the existing code does.

## Caveats

Two annotations look attractive but only get populated under solc
compilation paths puya-sol does NOT trigger:

- `internalFunctionIDs` — populated by
  `CompilerStack::annotateInternalFunctionIDs()` (line 626) which runs
  during `analyze()`. Looking again, this *does* run before puya-sol reads
  the AST, so it should be available. Probe before committing to refactor.
- `creationCallGraph` / `deployedCallGraph` — populated by
  `createAndAssignCallGraphs()` (line 625), also during `analyze()`.
  Should be available.

Both depend on solc finding "internal dispatch" — i.e., the contract has
to actually use internal function pointers and have its call graph
constructed. Empty contracts may have empty call graphs.

Solc's IR-generation step also touches these (`CompilerStack::generateIR`
line 1594) but we don't enter that path.

## Non-targets

Annotations that exist but where puya-sol's translation model doesn't have
a corresponding hand-rolled equivalent to delete:

- `ExperimentalFeature` set on SourceUnit — we ignore experimental features
  uniformly.
- `ContractDefinition::annotation().contractDependencies` — duplicates
  call-graph info; not useful as a separate target.
- `Block::annotation().externalReferences` for assembly — already used in
  `SolInlineAssembly.cpp`.
- `FunctionType::Options` (gas/value/salt set flags) — exposed via direct
  accessors on FunctionType which we already use.
