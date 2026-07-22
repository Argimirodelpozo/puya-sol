# possible_solc — unadopted solc facilities worth integrating

2026-07-22. Survey of vendored-solc (`solidity/`, 0.8.35) machinery puya-sol does not yet use,
ranked by correctness-per-effort. Each item is tied to a bug class we have already paid for.
Adoption status verified by grep: apart from `ConstantEvaluator` (via `SolcConstFold`) and the
basic layout/type queries, none of the below is referenced in `src/builder` today. Successor to
the deleted `solc-todo.md` / `annotations.md` planning docs and the post-AST-passes survey
(2026-07-11); items confirmed unusable there (IRGenerator, libsolidity/codegen, libevmasm
optimisers — all EVM-opcode-coupled) are not repeated here.

---

## Tier 1 — direct bug-class killers, cheap (~a day each)

### 1. `evmasm::SemanticInformation` + `yul::SideEffectsCollector` — effect tables for asm
> **✅ ADOPTED 2026-07-22 (v465)** for the memory-clobber classification: `s_memClobberers`
> hand-list replaced by `SemanticInformation::memory(instr) == Write` (CoreTranslation.cpp).
> `SideEffectsCollector` remains available for future Yul-expression-level uses.
`solidity/libevmasm/SemanticInformation.h` — authoritative per-opcode
`memory/storage/transientStorage/otherState(Instruction)` read/write classification.
`solidity/libyul/optimiser/Semantics.h` — `SideEffectsCollector` classifies whole Yul
expressions/blocks (`movable()`, `sideEffectFree()`, `containsMSize`).

**Replaces:** AssemblyBuilder's hand-maintained "untrackable memory writers" opcode list, the
const-cache kill rules (`invalidateMemConstants`), and parts of AsmScan gating.
**Bug class killed:** N1 stale compile-time caches (fable-review-3 Critical 2 existed because
the hand lists drift). Every future builtin handler becomes effect-correct automatically.

### 2. `Type::calldataEncodedSize()/calldataHeadSize()` — derive the `__cd_blob` layout
> **✅ ADOPTED 2026-07-22 (v467).** Head layout + value widening from declared solc types
> (paramSolTypes plumbed through FunctionContext). Found live bugs: blob assumed one head word
> per param (disagreeing with the map for static aggregates); signed sub-word head words
> zero-extended; bytesN right-aligned; sub-word dynamic elements kept ARC4 width. The
> CONSTANT-offset map path needed the same word semantics. Guard test_asm_cd_layout.
The synthetic-calldata blob must be byte-identical to EVM ABI encoding; we hand-compute the
head/tail offsets today.

**Bug class killed:** the calldata-pointer asm family and the asm-param-as-memory-offset bug
(a wrong calldata head-offset constant) were exactly this arithmetic going wrong. Deriving
offsets from solc's own ABI type math removes the class. Note: ARC4 sizes differ by design —
this applies ONLY to the EVM-faithful calldata transport, not ARC4 encoding.

### 3. `FunctionCallGraph` — transitive param-mutation closure
`annotation().creationCallGraph/deployedCallGraph` are computed by solc per contract.

**Closes:** `ParamMutationDetector`'s documented residual ("mutation via passing the param on
to ANOTHER mutating callee is not tracked — needs call-graph closure") = a silent write-back
drop. A transitive walk over the solc edges closes it.
**Bonus:** reachability for the uros splitter (only emit reachable subroutines) and dead
internal-function pruning → program size.

### 4. `FunctionType::externalIdentifier()` + `ContractDefinition::interfaceFunctionList()`
Canonical external-interface enumeration + selector math.

**Replaces:** hand-assembled signatures (the `__postInit(...)` string built in
SolNewExpression; getter signatures in PublicGetterBuilder).
**Bug class killed:** signature-derivation drift — the keyed-getter selector mismatches (H15b)
were caller and callee deriving signatures independently. Enumerate from solc's canonical
list, hash sha512_256 on our side; caller/callee agree by construction. (Keccak
`externalIdentifier()` itself is still useful for the EVM-literal magic values —
Error(string).)

---

## Tier 2 — high ceiling, needs design

### 5. Yul `OptimiserSuite` subset as a pre-pass on asm blocks  ⭐ biggest code savings
`OptimiserSuite::runSequence(abbrevs, Block&, repeatUntilStable)` is fully standalone (no EVM
tail). Prelude: `Disambiguator` (returns a NEW tree) + `"hgfo"`
(FunctionHoister/Grouper/BlockFlattener/ForLoopInitRewriter). Useful steps: LoadResolver `L`,
ExpressionSimplifier `s`, StructuralSimplifier `t`, CSE `c`, UnusedPruner `u`,
DeadCodeEliminator `D`, SSATransform `a`.

**Value:** canonicalizes input shapes so our pattern-matchers (keccak idioms, mstore folds,
calldata transport) see far fewer variants; could eventually DELETE the hand-rolled
`m_localConstants` / mem-content const tracking — LoadResolver does the same thing
flow-sensitively and soundly.

**⚠️ Known hazard (confirmed in the 2026-07-11 survey):**
`InlineAssembly::annotation().externalReferences` keys are raw `yul::Identifier const*` into
the pre-optimisation AST — Disambiguator/rewrites dangle them (this is why solc's legacy
codegen skips optimising asm blocks with external refs). Mitigation: pass Solidity-referenced
names as reserved/externallyUsed identifiers, then RE-RESOLVE externalReferences BY NAME
post-optimisation. Scope as its own multi-session project.

### 6. `Type::isImplicitlyConvertibleTo` asserts inside TypeCoercion
> **✅ ADOPTED 2026-07-22 (v466)** as `TypeCoercion::assertImplicitlyConvertible` at the five
> sites holding both solc types (signExtendSignedWiden, var-decl init, plain assignment,
> internal-call args, binop commonType). Lesson: bare isImplicitlyConvertibleTo is stricter
> than solc's CALL-SITE rules — acceptance is raw-pair OR memory-normalized-pair (storage
> copies convert elements raw-only; calldata params take memory args normalized-only);
> FunctionType pairs skipped. Zero corpus trips.
At every `implicitNumericCast` / `coerceForAssignment`, assert solc agrees the conversion is
legal. Several past bugs (dropped sign-extends, wrong widening) would have failed at COMPILE
time instead of surfacing as runtime divergences. ~One day; pure defense-in-depth.

### 7. `StorageLayout::generate(contract, location)` differential in the test harness
Our slot model mirrors Solidity layout for packed/fixed arrays. Auto-compare our slot
assignments against solc's canonical layout JSON in the harness (instead of hand-written
`storage_boundary_*` expectations) — a standing tripwire for layout drift. Also covers
transient storage slots.

### 8. `CFG` / `ControlFlowGraph::functionFlow()` — one true walker (review theme T5)
`constructFlow(root)` then per-function CFG with `variableOccurrences` (run
`ControlFlowRevertPruner` first for revert edges). Could consolidate the four partial
tree-walkers (`ParamMutationDetector`, `augmentReturns`,
`augmentMethodForMutatedMemoryParams`, `forEachReturnStatement`) onto solc's canonical
"where are the exits / what is assigned" answers. Medium effort; kills the walker-drift class
(H6, H15c, M27 gaps all came from partial walks).

---

## Tier 3 — features, not compiler correctness

### 9. SMTChecker passthrough
Forward `--model-checker-*` to the vendored `libsolidity/formal` analysis: free formal
checking for USER contracts (puya-sol inherits it by construction since analysis runs on the
same typed AST). Zero compiler-correctness value; nice flag to expose.

### 10. `@custom:` natspec tags as puya-sol hints
`DocStringTagParser` already surfaces `@custom:...` annotations on declarations. Use for
puya-sol-specific directives with zero new syntax: storage placement (box vs app-global
override), per-function opcode-budget hints (`ensure_budget` without CLI flags), splitter
grouping.

---

## Suggested attack order
1 (effect tables) → 2 (calldata layout) → 3 (call-graph closure) → 6 (coercion asserts), each
roughly a day and independently landable with the usual zero-regression gate. Then 7/8 as
harness/consolidation work, and 5 as its own scoped project.
