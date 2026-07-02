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
- solc already hard-errors on constant expressions that overflow their type — so a
  successful `tryConstantValue` at a declared type is by definition in range. Document and
  rely on this; do not re-check.

**Expected wins:** retire the C1/C2-adjacent fold bugs permanently; delete ~4 bespoke fold
sites; constants arrive earlier so downstream code (e.g. shift-amount clamps, budget) sees
literals it can lower cheaply.

---

### ✅ AGREED START — 2. Consume solc's effect annotations (`isPure`, mutability) for sequencing decisions

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

### 3. Derive EVM-canonical signatures from solc `externalSignature()` (proposed)

The cross-contract selector-mismatch family (signed struct/array naming `7cc1dc7a7c`, uint512
tuples `3c4dcf3bf4`) came from hand-building type-name strings in two conventions
(`intSelectorName` vs `nestedArc4Name`). We call `externalSignature()` in only 5 places. Keep
ARC4-specific naming ours, but derive the canonical Solidity type-name spine from solc instead
of reconstructing it. Medium effort; kills a demonstrated bug family at the root.

### 4. Consume solc's storage layout for the asm-visible surface (proposed)

The open packed/boundary `__dyn_storage` divergences (storage_boundary_* fails) live exactly
where we re-derive slot/offset/packing by hand. solc emits the authoritative layout
(`storageLayout`). Using it as the single source for the blob's geometry removes the drift
half of that problem; the full-width-only model question remains separate.

### 5. Retire twin paths; Yul consumption considered-and-rejected (proposed)

Kill `ModifierBodyInliner` and standardize on the (viaIR-style) chain builder unconditionally —
it was immune to both 2026-06 inliner bugs. Audit for remaining legacy/viaIR forks. The maximal
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

An `OperandPlan` / RAII `PrePendingScope` owning left-to-right order, exactly-once
materialization (gated by item 2's `isEffectFree`), and branch-scoping of pre-statements.
Retrofit binary-op/ternary/short-circuit/shift builders. Eliminates C1 as a class.

### 8. Finish the type-carrier consolidation (proposed; continues 3461ceee38/aceadac8aa)

Push `SolIntType` outward: helpers taking `{bits,isSigned}` pairs, the `paramBitWidths` side
channel, and eb builder constructors asserting `expr->wtype == typeMapper.map(solType)` (which
would have made the bytesN unsized bug a compile-time impossibility). Target: the 88
`dynamic_cast<IntegerType>` sites trend toward the carrier.

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
