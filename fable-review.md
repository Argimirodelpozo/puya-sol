# fable-review — architecture improvements for maintainability & correctness

2026-07-02. Derived from the bug ledger of the last ~6 weeks (semantic suite + differential
fuzzing campaigns): each recurring bug class below points at a missing structural discipline,
not a one-off mistake. Items 1–2 are the **agreed starting point**; the rest are captured for
ongoing discussion. Codebase size for scale: ~54.6k lines of C++ under `src/`.

---

## Part I — the recurring bug classes (evidence)

| # | Class | Paid-for examples | Structural cause |
|---|-------|-------------------|------------------|
| C1 | Evaluation order / effects | short-circuit RHS pre-stmts hoisted out of the branch (`5a1f5810ad`); ternary-operand SE false-revert (`cd9d91ccfa`); struct-field inc/dec double-eval; shift saturation guard lazily skipping a reverting value (fixed via eager comma-bind) | Effects sequenced by hand at every site: `prePendingStatements` touched in **34 files**, **15** `makeSingleEvaluation` + **30** `makeEvalOnce` + **12** `makeCommaExpression` sites, each a local re-solution of "evaluate once, in order, scoped to this branch" |
| C2 | Type-carrier drift | 6 stringly `substr(0,3)=="int"` sites (retired, `3461ceee38`); bytesN binop results carrying **unsized** wtype → narrowing no-op'd (`de2b25aa38`); signedness reconstructed per-site | Type identity lives in 3 places (solc `Type*`, AWST `wtype`, ARC4 alias string) with no invariant tying them; **88** `dynamic_cast<IntegerType>` sites still re-derive what a carrier should carry |
| C3 | Twin lowering paths that drift | two modifier impls (both inliner bugs `3e7394074b`/`845fc1a0b8` were in the path the chain-builder was immune to); 5 `promoteToBiguint` copies (now 1); 2 signed div/mod impls (now 1); slot-0 local-vs-scratch memory reads (the honk saga); BinaryOpBuilder's hand-rolled guard-less shift (now shared) | No "one canonical helper" rule; parallel paths accrete and then diverge |
| C4 | Schema-invalid AWST caught too late | puyabug #8: `AssignmentExpression.target = IntegerConstant` (asm `.slot` rebind), biguint `IndexExpression.base` — die in the backend as `deserialization failed: 'IntegerConstant'`, zero context | The frontend has no emission-time check of the schema the backend implicitly demands |

---

## Part II — the items

### ✅ AGREED START — 1. Adopt solc's `ConstantEvaluator` as the *only* constant folder

> **STATUS: COMPLETE** — slice 1: SolcConstFold.{h,cpp} (foldAnnotated + hoisted constantVarEvmWord),
> unary/binary sol-ast folding unified, the eb negate fast-path DELETED (its removal exposed a
> latent uint64/biguint wtype mismatch on constant operands of biguint-backed intN, now fixed with
> a promote at the path entry — caught by constantEvaluator::test_rounding). Slice 2: case-(b)
> foldTyped — intN-typed const-var expressions fold via ConstantEvaluator::tryEvaluate under an
> EVERY-NODE-in-range guard (walker over a strict node whitelist; conversions/calls never fold).
> Guard test_const_var_fold; AWST-verified: happy paths emit bare constants, the traps (`-M`,
> `(P<<1)>>1`, unchecked `P*3`) correctly take the runtime chains. EMPIRICAL CORRECTION to the
> risk note below: solc hard-errors CHECKED BINARY constant overflow at compile time, but lets
> UNARY negation overflow and SHIFT/unchecked-mul truncation through to runtime — the in-range
> guard IS load-bearing there; "do not re-check" was wrong.

**Goal:** our builders never fold constants themselves; they ask solc for the value first and
only lower the non-constant residue. Deletes code, deletes a bug class solc already solved.

**Current state (measured):**
- Exactly **one** integration exists: `src/builder/sol-ast/stmts/SolInlineAssembly.cpp`
  (`ConstantEvaluator::tryEvaluate(*initExpr)` — NB `tryEvaluate` is a fork-added static in
  `solidity/libsolidity/analysis/ConstantEvaluator.h:63`). Its comments already document the
  edge cases learned the hard way: bool handling (`constantToTypedValue` covers rationals
  only), negative literals, address/bytesN literals that the evaluator won't fold.
- Meanwhile we maintain our own ad-hoc folds/constant fast-paths in at least:
  `SolIntegerBuilder.cpp` (the unary negation fast-path — site of the `-type(intN).min`
  soundness bug `80169b6a8a`), `assembly/SignedOps.cpp` (IntegerConstant special-casing,
  lines ~165-175), `assembly/PrecompileDispatch.cpp`, `itxn/InnerCallHandlers.cpp`,
  `calls/SolRequireAssert.cpp` (constant-condition handling), plus scattered
  `dynamic_cast<awst::IntegerConstant*>` peeks at already-built AWST.

**Design sketch:**
- One helper, e.g. `src/builder/sol-types/SolcConstFold.{h,cpp}`:
  `std::optional<TypedValue> tryConstantValue(solidity::frontend::Expression const&)`
  wrapping (a) `annotation().type` being `RationalNumberType` (compile-time constant of
  rational type → `literalValue()`), (b) `ConstantEvaluator::tryEvaluate` for constant-typed
  expressions and const-var references, (c) the bool/address/bytesN quirks already solved in
  SolInlineAssembly — hoisted out of there so both share.
- Builders consult it BEFORE building operand AWST; on a hit they emit the folded constant at
  the DECLARED type's semantics (mask/two's-complement per width — reuse `SolIntType`).
- Migration is site-by-site, each gated by the suite + the arith fuzz axes; the
  SolInlineAssembly call site becomes just another consumer of the shared helper.

**Semantic risk notes (the part to get right):**
- **Folding must never delete a revert.** Two paid-for lessons: `-type(intN).min` (the fold
  fast-path skipped the overflow check) and the missing-revert-under-fold class
  (`test_dce_reverting_subexpr*`, puyabug #9): a constant fold that swallows a
  runtime-reverting subexpression is a soundness bug. Rule: fold ONLY when solc evaluates the
  ENTIRE expression (solc refuses when a subexpression isn't compile-time constant), never
  fold "around" a runtime operand.
- solc's rational arithmetic is arbitrary-precision; conversion to the declared type is where
  the semantics live (truncation/sign). Keep that conversion in ONE place (the helper),
  expressed via `SolIntType`.
- ~~solc already hard-errors on constant expressions that overflow their type — rely on
  this; do not re-check~~ **DISPROVEN by probe (2026-07-02):** solc hard-errors only CHECKED
  BINARY constant overflow (`(P+P)-P` rejects, even under unchecked); unary negation
  (`-M` with M=int8.min) and truncating ops (`P<<1`, unchecked `P*3`) compile and carry
  runtime semantics. foldTyped therefore range-checks EVERY integer-typed node itself.

**Expected wins:** retire the C1/C2-adjacent fold bugs permanently; delete ~4 bespoke fold
sites; constants arrive earlier so downstream code (e.g. shift-amount clamps, budget) sees
literals it can lower cheaply.

---

### ✅ AGREED START — 2. Consume solc's effect annotations (`isPure`, mutability) for sequencing decisions

> **STATUS: helper + first adoption LANDED** — SolcConstFold::isEffectFree (duplication-only license
> documented at the API; deliberately no canElide()). First site: require/assert skips the EvalOnce
> wrapper for pure leaf-var conditions. FINDING from the site audit: most existing SE/EvalOnce sites
> wrap for REUSE (compute-once is beneficial regardless of purity), so purity alone changes fewer
> decisions than the raw ~57-site count suggests — the broader win is coupled to item 7's
> OperandPlan, where isEffectFree becomes the materialization gate.

**Goal:** stop guessing whether an operand can have effects. solc's TypeChecker already
computed `annotation().isPure` (also `isLValue`, `willBeWrittenTo` —
`solidity/libsolidity/ast/ASTAnnotations.h:284-288`) for every expression. We currently use
them **zero** times.

**Where it plugs in (the ~57-site inventory):** every `makeSingleEvaluation` (15),
`makeEvalOnce` (30), and comma-binding (12) decision is an answer to "could evaluating this
twice / out of order / not at all be observable?". Today we answer it two bad ways:
- **Defensively** — SE-wrap everything → opcode cost + AWST bloat (and SE ids are the fragile
  currency of several past splitter bugs);
- **Forgetfully** — the C1 bug row: double-eval and skipped-eval bugs the fuzzer keeps finding.

With `isPure` available, the decision becomes: `isPure(expr)` → duplicate/reorder freely, no
SE; else → mandatory SE/pre-binding. Both failure directions become structural.

**The critical nuance (must be in every use of this API):** solc's `isPure` means *no state
read/write and no environment dependence* — it does **not** mean *cannot revert*. `a / b` with
constant-free operands is `isPure` yet reverts on `b == 0`. So:
- For **evaluate-once** decisions (SE/EvalOnce): `isPure` is the right and sufficient gate —
  evaluating a pure expression twice is unobservable (cost aside).
- For **evaluate-at-all / reorder-across-control-flow** decisions (the shift saturation guard,
  short-circuit scoping, anything that might SKIP an operand): purity is NOT sufficient —
  can-revert matters, and puyabug #9 shows the backend also treats div/mod as droppable. Rule:
  operand evaluation may never be made conditional on anything Solidity doesn't make it
  conditional on, purity notwithstanding. `isPure` only licenses *duplication*, never *elision*.

**Design sketch:**
- Tiny helper `bool isEffectFree(solidity::frontend::Expression const&)` returning
  `*annotation().isPure` (SetOnce — assert it's set; it always is post-TypeChecker), plus a
  documented `canElide()` that is deliberately NOT provided (see nuance above).
- Adopt at the ~15 SingleEvaluation sites first (lowest risk: the change only ever REMOVES
  wrapping for provably-pure operands or ADDS it where missing); then the EvalOnce sites.
- Each adoption slice gated by: the side-effect-once fuzz fixtures
  (`exprs_sideeffect`, `compound_*`, `ret_postinc`, `inc_iso`), the operators/arithmetics
  suites, and one generative campaign phase (A + B axes).

**Expected wins:** C1 shrinks structurally; measurable opcode savings wherever defensive SE
wrapping drops (SE = scratch traffic); prepares the ground for item 7 (the sequencing
abstraction) by making effectfulness a queryable property instead of folklore.

---

### 3. Derive EVM-canonical signatures from solc `externalSignature()` (proposed — SCOPED 2026-07-02)

**Scoping answer to "can we do this without breaking the chosen encode schema": yes, because the
schema has two regimes and only one of them may consume solc's string.**

- **Regime A — "canonical Solidity name × sha512_256"** (custom errors, events, `.selector`,
  interfaceId): the signature STRING already comes from solc's `externalSignature()` and only
  the HASH is ours (MethodConstant/ARC-28) — the 5 existing call sites ARE this item's pattern,
  already practiced. Remaining work: audit for stragglers that hand-build such strings.
- **Regime B — "ARC4/backing-width names × sha512_256"** (method registration + cross-contract
  and inner-call selector computation: `intSelectorName`/`nestedArc4Name`/`wtypeToABIName`):
  these strings must match what puya's router registers (backing widths, e.g.
  `packP(uint64,uint64)` for int64 params) — `externalSignature()`'s declared-width string
  would BREAK routing outright and must never be used here. What solc can still supply is the
  type-DECOMPOSITION skeleton (struct→tuple expansion, enum/UDVT/contract canonicalization)
  under OUR leaf-name renderer; `7cc1dc7a7c` already canonicalized most paths onto
  `nestedArc4Name`, so the residual value is modest.
- **Migration protocol if/when regime B is consolidated:** equality-gated — compute the new
  derivation alongside the old, hard-error on any mismatch, run the full suite (every arc56
  method signature in the corpus is the oracle), then swap and delete. "Schema unbroken"
  becomes a verified property, not a hope.

### 4. Consume solc's storage layout for the asm-visible surface (proposed)

The open packed/boundary `__dyn_storage` divergences (storage_boundary_* fails) live exactly
where we re-derive slot/offset/packing by hand. solc emits the authoritative layout
(`storageLayout`). Using it as the single source for the blob's geometry removes the drift
half of that problem; the full-width-only model question remains separate.

### 5. Retire twin paths; Yul consumption considered-and-rejected (proposed)

~~Kill `ModifierBodyInliner` and standardize on the chain builder unconditionally~~ **PREMISE
DISPROVEN by audit (2026-07-03): NOT a clean deletion.** `m_viaIR=false` by default, so
`inlineModifiers` (ModifierBodyInliner) is the DEFAULT path every modifier-using contract
compiles through; `buildModifierChain` is the opt-in `--via-yul-behavior` MINORITY path and
covers only viaIR *method* modifiers (one call site). `inlineModifiers` is ALSO the sole path
for constructor modifiers (both base + derived, ApprovalProgramBuilder) AND library/free-function
modifiers — even under viaIR. So retiring it means making the chain builder the default AND
extending it to constructors AND libraries: a codegen change for nearly every contract, high
regression surface, multi-session — NOT a LOC-negative quick win. Deferred; the two June inliner
bugs are already fixed, so there's no urgency. Audit for remaining legacy/viaIR forks. The maximal
version — consuming solc's Yul IR wholesale — is explicitly rejected for the main pipeline:
Yul arrives with semantics pre-lowered (attractive) but is irreducibly EVM-shaped (256-bit
words/linear memory/slot storage) — adopting it forfeits uint64-native math, box storage and
ARC4, i.e. the entire AVM budget story (honk). We already extract Yul's correctness value more
cheaply by using real solc+EVM as the differential-fuzzing oracle.

### 6. AWST emission validator in the frontend (proposed — highest correctness leverage/line)

After building each function body, walk the emitted tree asserting the backend's implicit
schema: assignment targets are Lvalue-class; expression wtypes sized/consistent with the mapped
solc type; index bases indexable. Every puyabug-#8-class escape becomes a frontend error WITH a
source location instead of a backend `KeyError` one-worder. Walking skeleton exists
(`src/awst/Clone.{h,cpp}` covers all ~63 node types). Also the honest resolution of the
reverted `.slot` hard-error attempt: validate what was *emitted*, not what might be.

### 7. One effect-sequencing abstraction (proposed; builds on item 2)

> **STATUS: v1 LANDED (first slice).** OperandPlan = two primitives on ContractContext (it owns
> prePendingStatements): `buildScopedOperand(build, capturedOut)` runs a build callback and MOVES
> any pre-statements it pushed out of the flat list into capturedOut (so the caller gates them
> behind the operand's condition instead of flushing unconditionally — THE C1 invariant), and
> `makeScopedResultBlock(preStmts, target, value)` wraps captured stmts + a result-assign into the
> block both sites need. Retrofitted the ternary (SolConditional, 3 hand-rolled captures) and
> short-circuit (trySolShortCircuit, 1) — the two paid-for C1 sites (cd9d91ccfa, 5a1f5810ad).
> Behavior-preserving (AWST byte-identical modulo source-location).
>
> **Slice 2 LANDED d10296365b (materialize-once half):** FINDING — the primitive already EXISTED
> as `makeEvalOnce` (Node.h), which centralizes "skip trivially-duplicable leaf (var/constant/
> already-SE), else SingleEvaluation". Documented it as OperandPlan's materialize-once primitive
> and consolidated the 5 shift/amount raw `makeSingleEvaluation` sites (BigUIntMathHelpers x2,
> BitwiseShiftOps x3) onto it — byte-identical for non-trivial operands, safe SE-skip on constant
> amounts. isEffectFree gating (item 2) has limited reach at these AWST-level sites (no solc expr
> in scope); makeEvalOnce's AWST-shape leaf check is the right tool there — confirms item 2's
> "broad isPure win is narrower than the site count suggests". NEXT: migrate the remaining ~6 raw
> SE sites onto makeEvalOnce + binary-op operand ordering.

An `OperandPlan` / RAII `PrePendingScope` owning left-to-right order, exactly-once
materialization (gated by item 2's `isEffectFree`), and branch-scoping of pre-statements.
Retrofit binary-op/ternary/short-circuit/shift builders. Eliminates C1 as a class.

### 8. Finish the type-carrier consolidation (proposed; continues 3461ceee38/aceadac8aa)

> **STATUS: IN PROGRESS.** The assertion half is PREMISE-DISPROVEN; the carrier-migration half
> is landing incrementally.
>
> **Part (a) — the `expr->wtype == typeMapper.map(solType)` assertion at the eb builder seam:
> ABANDONED (premise doesn't hold, empirically).** Prototyped a fail-loud net in
> `BuilderRegistry::tryBuildInstance`. A blanket scalar check false-positives on CONVERSIONS
> (`address(uint160)` carries biguint, `bytesN(uint)` carries uint64 — the builder legitimately
> wraps the source expr to convert it). Narrowing to "FixedBytes wrapping UNSIZED `bytes`" (the
> exact de2b25aa38 signature) still fires on GREEN tests: `abi.encodeWithSignature` builds
> `bytes32` head/tail WORDS from unsized `bytes` internally, which is correct because that path
> never narrows. So unsized-bytes-into-bytesN is a LEGITIMATE internal shape, not a bug per se —
> the de2b25aa38 bug was specifically the narrowing no-op, already guarded by the
> `SolFixedBytesBuilder` retag. A construction-seam hard error only adds false positives. (Same
> shape of lesson as item 5: a plausible doc premise, disproven by probe.)
>
> **Part (b) — trend the ~88 `dynamic_cast<IntegerType>` sites onto `SolIntType`: LANDING.**
> Slice 1: TypeCoercion.cpp — all 5 sites migrated to `SolIntType::fromSol` (which already does
> the UDVT-unwrap + IntegerType-cast + {bits,isSigned}). Removed 5 hand-rolled unwrap+cast blocks
> incl. an `asInt` lambda that was `fromSol` reimplemented. Behavior-preserving (fromSol is
> identical logic); full suite baseline. NEXT: the other high-count files (FunctionBuilder 10,
> SolUnaryOperation 7, SolTypeConversion 7), then the `expr->wtype == map(solType)` DEBUG-assert
> as a `#ifndef NDEBUG` check (not a production hard-error) if still wanted.

Push `SolIntType` outward: helpers taking `{bits,isSigned}` pairs, the `paramBitWidths` side
channel. Target: the 88 `dynamic_cast<IntegerType>` sites trend toward the carrier. (The
constructor-assertion idea is retired — see STATUS above.)

### 9. Promote the fuzzing oracle out of WIP (proposed)

`tests/WIP/tiny-fuzzing-oracle` found essentially every recent bug and is still gitignored
scratch. Commit the harness + campaign driver (`fuzz_campaign.py`), give it a nightly seed
rotation. Makes the strongest correctness asset durable instead of session-local.

---

## Part III — sequencing & validation protocol

Suggested order: **1 → 2** (agreed) → 6 → 5-lite (modifier path) → 7 → 2's remaining sites →
8 → 3 → 4 → 9 opportunistically.

Every slice lands under the established gates: full semantic suite vs the current baseline
(fail-set identical or better; baseline at this writing: 44 failed / 1283 passed / 99 xfailed /
26 xpassed — the 44th is the intentionally-red `test_dce_reverting_subexpr_literal_folds`),
plus the relevant differential-fuzz axes (arithmetic/casts for item 1; the side-effect fixtures
+ generative A/B phases for item 2). Policy constraints observed throughout: no puya-fork
changes (backend-rooted residuals go to puyabug.md); open bugs stay regular fails, xfail only
for by-design divergences.

---

## Part IV — round-2 solc-integration candidates (2026-07-02 sweep)

A follow-up sweep for solc facilities we still re-derive, grounded in the code. Two
already-integrated findings first (no action): `annotation().commonType` is consumed
(SolBinaryOperation:140) and `FunctionCallKind` has 17 uses — the binop/conversion dispatch
already trusts solc there.

### 10. EVM-ABI shape oracles for the calldata/asm surface (rides with the calldata cluster)

`SyntheticCalldataOps.cpp:213` computes `headWords = _params.size()` by hand — the EXACT shape
of the recorded calldata-pointer failure ("blob/offset-map built from AUGMENTED params →
inflated headWords → off-end extract3", see the calldata-pointer-asm-model plan). solc owns the
authoritative EVM-ABI geometry: `Type::calldataEncodedSize()`, `calldataHeadSize()`,
`isDynamicallyEncoded()`, and `FunctionType` parameter layouts. When the calldata cluster
(~4-5 canonical tests) is attacked, the head/tail/offset math should come from these, not from
counting parameters. Small-medium; directly de-risks that workstream's known failure mode.

### 11. `resolveVirtual` / `VirtualLookup` for super & override resolution (standalone, LOC-negative)

> **STATUS: LANDED** (net −89 src lines). The three hand-rolled paths (name-keyed MRO chains — which
> mixed overloads into one chain — an AST-id fallback, and a separate explicit-base pass) are ONE
> solc-native resolution: `requiredLookup` discriminates the site, `resolveVirtual(mostDerived,
> searchStart)` resolves it. Emission is now deduped per TARGET (`f__impl_<targetId>`) instead of one
> copy per caller. ⚠️ API GOTCHA recorded the hard way: `resolveVirtual`'s `_searchStart` is
> INCLUSIVE — super resolution must pass the scope contract's SUCCESSOR via
> `ContractDefinition::superContract(mostDerived)`, exactly as solc's own codegen does; passing the
> scope itself resolves `super.f()` to the caller's own `f` (caught by test_super_in_constructor's
> diamond).

We use solc's `resolveVirtual` ZERO times and instead maintain our own
`contract/SuperCallResolution.cpp` (+ override handling in ContractBuilder /
ApprovalProgramBuilder). solc's resolver — `FunctionDefinition::resolveVirtual(mostDerived,
super)` with the `annotation().requiredLookup` (Static/Virtual/Super) discriminator — IS the
language definition of C3-linearized dispatch. Ours currently agrees (inheritance suite green),
but it is duplicated semantic logic of exactly the class this review targets, and the
replacement should DELETE more than it adds. Medium effort; gate with the inheritance +
modifiers + super categories and the cross-contract fuzz axis.

### 12. solc `CallGraph` for reachability (when the splitter work resumes)

Reachability closures are hand-rolled today (assembly UserFunctionOps + the splitter's
force-inline closure logic; see also rust-honk's "needs reachability analysis" note). solc
builds creation/deployed `CallGraph`s per contract (`ContractDefinitionAnnotation::
creationCallGraph` / `deployedCallGraph`) with external/internal call edges resolved through
virtual dispatch. When the uros-IR-port / splitter workstream resumes, consuming these instead
of re-walking the AST removes a whole analysis; also useful for pruning dead helpers before
size-capped chunking. Medium; belongs to that workstream, not standalone.

**Scope boundary (asked & settled 2026-07-02): `SubroutineReachability` is NOT this item's
target and should stay.** It is AWST-level output DCE — a 110-line transitive closure over the
EMITTED SubroutineCallExpressions, i.e. ground truth after lowering. Builders synthesize calls
with no source counterpart (ripemd160's shared body, funcptr dispatchers, asm user functions,
inner-call shapes — 6+ files emit makeSubroutineCall), and lowering also eliminates source
calls; solc's CallGraph sees neither direction. Swapping it in would prune unsoundly the moment
a synthesized call targets an otherwise-source-unreachable root. Item 12 targets the
SOURCE-level re-walks only (splitter force-inline closures, pre-chunk reachability) where the
analysis input genuinely is the Solidity AST.

### Not worth it (reviewed, rejected)

solc's CFG analyses (uninitialized-return checking — solc already enforces), `GasMeter`
(gas ≠ AVM budget), NatSpec/metadata, SMTChecker. Event/error selector derivation is already
covered by item 3's `externalSignature()` spine.
