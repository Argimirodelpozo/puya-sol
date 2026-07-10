# fable-review-2 — project review: refactoring, types, design, solc, test speed

2026-07-08. Successor to `fable-review.md` (2026-07-02). Grounded in four parallel code surveys
plus the failure-set/bug ledger of the intervening week. Codebase: **54.7k lines** C++ under
`src/`; suite baseline **44f / 1287p / 99xf / 26xp** (44th = intentional-red DCE guard).

---

## Part 0 — review-1 scorecard (what landed since 2026-07-02)

| Item | Status |
|---|---|
| 1 ConstantEvaluator as only folder | ✅ landed |
| 2 isPure/effect annotations | ✅ helper + first adoptions (narrower win than hoped, by design) |
| 5 retire twin modifier paths | ✅ **landed 2026-07-08** (chain-as-default `14e9b6ac38` + retParam threading; textual inliner now ctor/library-only) — review-1 called this "deferred, multi-session"; it took one |
| 7 OperandPlan | ✅ primitives landed; adoption partial (see D6) |
| 8 SolIntType carrier | 🟡 in progress: 88 → 61 `IntegerType` casts |
| 11 resolveVirtual | ✅ landed (−89 LOC) |
| 3 externalSignature | 🟢 effectively done (regime A uses it ×5; no stragglers found) |
| 4 storage layout, 6 AWST validator, 9 promote fuzzer, 10 calldata geometry, 12 CallGraph | ❌ open — 4 and 10 now map to **~60% of the remaining failing set** (see S1/S2) |

Metric deltas since review-1: raw `makeSingleEvaluation` 15→6, `makeEvalOnce` 30→41 (good — the
centralized primitive absorbed sites), `prePendingStatements` files 34→29, TODO/FIXME count: 4.

---

## Part I — THE convergent finding: one `AbiCodec` module (refactor × types × design)

Three independent surveys ranked the same thing first. The Solidity-type → {ABI/selector name,
ARC4 encode, ARC4 decode} mapping is implemented **many times** and the copies are *already
behaviorally divergent*:

- **7 hand-rolled type→ABI-name ladders**: `TypeCoercion::wtypeToABIName`,
  `SimpleSplitter::abiTypeName` (~95 lines, the most complete), `PureHelperExtractor::arc4TypeName`,
  `InnerCallHandlers::solTypeToARC4Impl/RetImpl + nestedArc4Name`, `SolExternalCall`'s inline
  lambda, plus lambdas in `AbiEncoderBuilder` and `SolEmitStatement`.
- **biguint is named three different ways by design** — `"uint512"` (SimpleSplitter, internal wire),
  `"uint256"` (4 other sites, ABI selector), and `wtypeToABIName`'s `default: _type->name()`
  fallthrough — kept aligned only by comments. `PublicGetterBuilder.cpp:579` hand-patches over it.
- **Two `encodeArgToBytes` twins have ALREADY DRIFTED** (`SolExternalCall.cpp:89` vs
  `InnerCallHandlers.cpp:110`): different biguint widths (exact N/8 trim vs unconditional
  32-byte pad), different uint64 padding (param-width-aware vs bare 8-byte itob), and the inner
  path has **no ReferenceArray/ARC4Struct/ARC4Tuple cases** at all — a latent revert class on
  inner calls with those arg types.
- **Three `buildMethodSelector` wrappers** independently assemble param/ret name lists; the two
  namers already diverged on enums (this week's `uint8`-vs-`uint64` selector bug was exactly this).
- **Four ARC4 encode entry points**: `AbiEncoderBuilder::arc4EncodeValues*`,
  `SolExternalCall::encodeArgToBytes`, `InnerCallHandlers::encodeArgToBytes`,
  `ReturnRewriter::encodeRet` — each re-derives the ARC4 target type from the Sol type.

Every selector-mismatch and tuple-width bug of the past month (enum selector, uint512-vs-uint256
tuple elements, biguint natural-width) is this seam. **Proposal:** one `builder/abi/AbiCodec`
(or grow `sol-types/Arc4Naming` + `abi/`) owning:
`selectorName(solType, position)` (position = param | return | nested — the intentional
divergences become explicit parameters, not parallel copies), `encodeTo(argExpr, solParamType)`,
`decodeFrom(bytes, solRetType)`. Migrate equality-gated (compute new beside old, hard-error on
mismatch, full suite as oracle — every arc56 signature in the corpus checks it), then delete.
Also fold `PureHelperExtractor::staticEncodedSize` into the existing shared
`computeEncodedElementSize`. **Est. −250–400 LOC and retires the single most productive bug
seam in the ledger. Do first.**

> **PROGRESS (2026-07-08, slices A+B landed):** (A) ONE sol-type→selector-name family —
> `eb::solTypeToArc4ParamName/ReturnName`; SolExternalCall's 35-line lambda copy deleted, all 3
> `buildMethodSelector` wrappers now share it; the inner path's `toString(true)` exotic fallback
> aligned on `nestedArc4Name`'s callee-published mapping. (B) the two DRIFTED `encodeArgToBytes`
> twins are ONE param-type-aware encoder (`InnerCallHandlers::encodeArgToBytes(ctx,arg,solType,loc)`);
> the fused `.call(abi.encodeCall(...))` shape now encodes at the target's DECLARED param types
> (fixes the latent biguint-32B-vs-uint128-16B inner-call revert + raw-asBytes arrays), the
> type-less `encodeWith*` shape passes nullptr (backing-width behavior preserved branch-for-branch).
> Equality-verified: typed path AWST byte-identical (10+ fixtures); full suite at baseline.
> **Slice C (2026-07-09): WType-side family DONE.** One canonical
> `TypeCoercion::wtypeToABIName(WType*, BareBiguintName)` — the splitters' silent
> uint512-vs-uint256 bare-biguint disagreement is now an explicit parameter; SimpleSplitter's
> `abiTypeName` (−90 lines) and PureHelperExtractor's `arc4TypeName` (−25) are one-line delegates.
> Equality-proven beyond the suite (splitter paths aren't suite-covered): full PoolManager uros
> split (44 MB AWST, 263 selector constants) + --deploy-pure-helpers run, both byte-identical.
> The SolEmitStatement event namer is documented DIFFERENT-BY-DESIGN (events collapse biguint-backed
> ints to "uint256" to match puya's ARC-28 registration) — not merged.
> **Slice D (2026-07-09): R1/D1 COMPLETE.** `AbiEncoderBuilder::buildARC4MethodSelector` (the
> FOURTH selector wrapper, with the dispatch-breaking "struct Name"/toString ladder) DELETED —
> its 4 callers (fn-pointer dispatch tables, `.selector`) use the canonical
> `InnerCallHandlers::buildMethodSelector`; all 37 fn-pointer fixtures byte-identical (the
> divergent branch was corpus-unreachable). The encode entry points resolved by DOCUMENTATION,
> not merging — the probe found they are already one-per-convention BY DESIGN (abi.* backing
> widths / ApplicationArgs declared widths / return-wire / ARC-28 events); the missing artifact
> was the map, now written at arc4EncodeValues as "THE ENCODE-CONVENTION MAP (do not add a fifth
> copy)". Final state: every name ladder unified or convention-anchored; ~-300 lines total.

---

## Part II — by axis

### R. Refactoring (reduce codebase) — realistic total ≈ −800–1200 LOC

| # | Refactor | Est. | Risk |
|---|---|---|---|
| R1 ✅ | The `AbiCodec` consolidation (Part I) — slices A-D landed, see Part I progress notes | ~−300 | done |
| R2 | **Generic AWST statement walker.** ~9 hand-rolled recursive walkers over Block/IfElse/WhileLoop (Termination ×3, Clone, ReturnRewriter::forEachReturnStatement, ModifierInliner::fixReturns, ModifierBodyInliner ×2, FunctionBuilder::SubroutineCallVisitor, AWSTBuilder ×2). A generic walker **already exists** (`splitter/AwstWalker.h`) but only serves the splitters — extend it with statement callbacks + in-place mutation support and point the rest at it | −150–250 | Med (two walkers mutate during traversal) |
| R3 | Splitter cross-dedup: shared `Arc4Sig` + router primitives across SimpleSplitter/FunctionSplitter/PureHelperExtractor (all three ARE live, behind separate CLI flags — none is dead) | −100–150 | Low |
| R4 ❌ | ~~`__arc4_` param-decode shim~~ **PREMISE DISPROVEN by probe (2026-07-08):** the "copies" share only the `__arc4_<name>` NAMING convention; the decode strategies genuinely diverge (FunctionBuilder: ConvertArray for dynamic arrays + sign-extension + deferred wtype for asm; PublicGetterBuilder: biguint-only via arc4UintCodec; ApprovalProgramBuilder: postInit-specific). A shared helper would need ~5 callbacks — not a simplification | 0 | — |
| R5 ❌ | ~~Shared Yul-tree walker~~ **PREMISE DISPROVEN by probe (2026-07-08):** solc's `yul::ASTWalker` IS linked and available, but each hand-rolled walker has DELIBERATE traversal quirks its defaults would silently change — UserFunctionOps' `scanLeave` must NOT recurse nested FunctionDefinitions (a `leave` there belongs to the inner fn) and skips ForLoop; AssemblyBuilder's call-graph scan threads per-function accumulator state. Behavior-preserving subclasses ≈ as long as the lambdas, with asm-semantics regression risk | 0 | — |
| R6 | Split the 556-line `SolInternalCall::buildSubroutineCall` into target-resolution + arg-coercion stages; split `TypeCoercion.cpp` (1100 lines = 3 responsibilities: value coercion / ABI naming (→R1) / default-value construction) | readability | Low |
| R7 | After D5 (ctor modifier convergence, deferred): delete `ModifierBodyInliner`'s flag+`while(true)` early-return machinery (~150 lines) — only reachable now via ctor/library modifiers with early returns | −150 | gated on D5 |

### T. Type system

| # | Improvement | Sites | Risk |
|---|---|---|---|
| T1 ✅ | ~~WType-side numeric-tier predicate, 187 sites~~ **PREMISE CORRECTED by census (2026-07-09):** the 187 lines are overwhelmingly single-tier singleton compares (`== biguintType()` ×93, `== uint64Type()` ×78) — already minimal idiomatic form; wrapping them is churn. The genuine re-spelled pattern (the is-native-integer PAIR) existed at 5 sites → `awst::isNumericWType` added, sites migrated, byte-identical | 5 | done |
| T2 ✅ | Delete the **6 verbatim UDVT-unwrap copies** of `fromSol`'s body (AWSTBuilder ×2, ApprovalProgramBuilder, ParamABIValidator, SolInlineAssembly, SolNewExpression) + migrate the ~6 enum-recast sites to the under-adopted `fromSolOrEnum` | ~12 | **Low — do first** |
| T3 | Migrate the remaining ~22 clean `fromSol` candidates (pairs-of-int conversion sites etc.); leaves only ~8 definitional/predicate casts (TypeMapper's own `map()`, SolcConstFold) which SHOULD stay raw | ~22 | Low |
| T4 ✅ | **Guarded bytes-view helpers** (landed as `awst::asBytesWType/fixedBytesLength/isDynamicBytes` in WType.h — WType-side, not a Sol-side carrier). PREMISE CORRECTION from implementation: the 2 "unguarded null-derefs" are guarded by invariant today (`bytesType()` IS a `BytesWType{nullopt}` and BytesWType is the only kind-Bytes class) — latent hazard, not live bug. 5 cast-combo sites migrated (StorageMapper, ApprovalProgramBuilder ×2, SimpleSplitter, ReturnRewriter); the helpers now enforce the invariant in one place | 5 of ~45 | Low-Med |
| T5 | `TypeCoercion` split (see R6) — unblocks R1 and breaks the header cycle that already forced `SolIntType::pow2NAndHalf` out-of-line | — | Low |

### D. Design

| # | Improvement | Evidence | Risk |
|---|---|---|---|
| D1 ✅ | `AbiCodec` (Part I) — slices A-D landed | — | done |
| D2 ✅ | **`ReturnRewriter` redesign** — CHARACTERIZATION LANDED + its 2 surfaced BUGS FIXED (2026-07-09): the WIRE-RETURN-TYPE spec table is written at the top of the file (oracle fixture tests/WIP/return-wire-oracle/, regenerate+diff gates change). The table surfaced two live cross-contract-return bugs, both now **FIXED & guarded** (test_modifier_dyntuple_return, fuzz_crosscall 8/8, negatives): (a) biguint element in a DYNAMIC-element tuple left "uint512" vs caller "uintN" — Pass 3's `allStatic` guard removed, biguint wraps in ANY tuple; (b) MODIFIER'D (chain-lowered) biguint/signed returns published "uint512" — new `encodeChainDispatchReturn` encodes the outer dispatch return + `buildModifierChain` now threads the promoted `method.returnType` (a fresh map() gave int64→uint64 → "Tuple type mismatch"). BUILD-TIME-ENCODING re-architecture IN PROGRESS (A1 218d50bfe5 scalars byte-identical, A2 9e62008a1f tuples semantic-identical — both gated, fuzzer-clean, +latent signed-ternary bug fixed): return values now ARC4-encoded at CONSTRUCTION (SolReturnStatement + FunctionBuilder synth) via TypeCoercion::encodeReturnValue + a plan plumbed through FunctionContext, instead of the post-pass walking built AWST. COMPLETE (A3 ff6b1fe1df): mask/array/asm moved to build-time too, then the now-DEAD non-chain passes 2 & 3 DELETED (ReturnRewriter.cpp 623→491). Non-chain ABI returns are encoded at construction (builder owns it, using solc's return types); the post-pass runs ONLY for chain-lowered fns (Pass 1/4/5/6 + encodeChainDispatchReturn — the chain is a legitimately-later phase). Every shape (scalar/tuple/mask/array/asm incl overflow + signed negatives) fuzz-clean vs live EVM, 30-fixture AWST semantic-identical, failset identical. The return-wire bug class is structurally closed for non-chain. Pass 4's !chainLowered branches now dead-but-harmless (future tidy). Earlier COMPUTE-ONCE half LANDED 15347da6db (single computeReturnPlan → per-element {wireType,isSigned,bits,encoded}; passes 2/3/4 + chain dispatch all read it, the 'pass 4 re-does pass 3' copy-paste is gone; AWST byte-identical on 30 return-heavy fixtures). WALK-ONCE half DEFERRED by design: the 3 shape-walks carry output-load-bearing quirks (spill temp __ret_tmp vs __rettuple; ternary-rewrite only in the unsigned path) so merging changes AWST bytes — cosmetic gain, ABI-output-contract risk, needs a looser (non-byte-identical) gate | ReturnRewriter.cpp | High |
| D3 ✅ | **`awst::NameGen`** (thread_local per-prefix counters + resetAll at ContractBuilder::build) replaced 45 statics; contract names now depend only on the contract's own content (order-independent multi-contract output; parallel-safe). Single-contract compiles byte-identical; one latent bare-read bug fixed en route (SolNewExpression) | 45 | done |
| D4 | Collapse `ContractBuilder`'s nine `m_current*` scratch members into an owned `FunctionTranslationCtx` (RAII-swapped per function) — the struct already exists and is snapshot-copied field-by-field by `makeFunctionCtx()`; adding per-function state currently means editing 3 places | ContractBuilder.h:45-76 vs 150-158 | Low-Med |
| D5 | Converge **constructor** modifier lowering onto the chain (the last twin lowering). ReturnRewriter pass 6 already carries a special case for the textual path leaking into returns. High risk (base-ctor arg order, postInit) — **defer**, gate with the dispatch fuzzer; unlocks R7 | ApprovalProgramBuilder.cpp:587-805 | High |
| D6 | Finish OperandPlan adoption: `buildScopedOperand` used at only 2 sites (ternary, short-circuit); any other conditionally-evaluated operand that pushes pre-statements still flushes unconditionally — same latent C1 class. Also unify the 4 flush sites (SolControlFlow hand-merges pre+post) onto `appendPendingTo` | ContractContext.h:151-185 | Low-Med |
| D7 | `m_viaIR` is vestigial: ONE live branch left (ctor state-var init order, ApprovalProgramBuilder.cpp:646). Rename to `m_legacyCtorInitOrder`, localize, drop from every forwarding signature | — | Very low |

### S. solc integrations (what remains after 1/2/3/5/11 landed)

| # | Item | Evidence | Payoff |
|---|---|---|---|
| S1 | **Storage layout from solc for the asm/packing surface** (review-1 item 4). solc's authoritative layout (`storageLayout`, `StructType::storageOffsetsOfMember`, `Type::storageBytes` — already partially consumed in TransientStorage/StorageMapper) vs our hand-derived packing | **~17 of the 43** real remaining failures are storage-boundary/packing/slot tests | The single biggest failure-count lever left |
| S2 | **Calldata ABI geometry from solc** (review-1 item 10): `Type::calldataEncodedSize/calldataHeadSize/isDynamicallyEncoded` for the `__cd_blob` offset map (the recorded blocker was exactly hand-counted `headWords`) | **9 of 43** remaining failures are calldata-asm | Second biggest lever; rides the calldata cluster |
| S3 | AWST emission validator (review-1 item 6) — still the highest correctness-leverage-per-line idea; `Clone.cpp`/`AwstWalker` provide the walking skeleton (synergy with R2) | puyabug #8 class | Fail-loud at frontend with source locations |
| S4 | solc `CallGraph` for source-level reachability — parked with the splitter workstream (unchanged) | — | — |
| S5 | **Promote the fuzzing oracle out of `tests/WIP`** (review-1 item 9, still open). It found essentially every bug of the past month (incl. this week's entire modifier arc) and remains untracked scratch. Commit drivers + probes + `fuzz_dispatch.py`, wire a nightly seed rotation | — | Durability of the strongest correctness asset |

### P. Testing times (empirical; suite = 419–1013s single-threaded today)

Per-test anatomy (measured): 1456 tests ≈ 1448 deploys (1:1 — **no** re-deploy waste to
mine) + ~4,600 calls ≈ **40,000 HTTP round-trips**. Devmode is already ON. Compile cache is
already excellent. The levers, ranked:

| # | Change | Impact | Effort |
|---|---|---|---|
| P1 ✅ | **Reset the localnet before full runs.** Root cause of the 2.4× variance FOUND: the running localnet has accumulated 316k rounds / **2.1 GB ledger** over days; everything (esp. simulate) slows as it grows. The reset hook already exists (`conftest.py`, `PUYASOL_LOCALNET_RESET=1`) — make it the default for full runs | kills the 419→1013s drift | **Trivial** |
| P2 ✅ | **Gate `populate_app_call_resources`**: today a full SIMULATE precedes *every* execute (~4,600 extra program runs against the growing ledger). Only needed when boxes/extra-apps/accounts are passed: `if boxes or extra_apps or extra_accounts:` | est. **25–40%** | Low |
| P3 ✅ | Re-enable suggested-params caching (`set_suggested_params_cache_timeout(0)` at localnet.py:24 forces ~10k fresh `GET /params`; fees are overridden manually anyway → 30–60s TTL is safe) | est. 5–15% | Trivial |
| P4 | Deploy from the already-emitted `.bin` instead of 2× `algod.compile` per deploy (keep the algod path only when `TMPL_` substitution is needed) | ~2,900 RT | Med |
| P5 | Atomic-group create+fund (one submit+confirm instead of two); devmode-aware confirm (skip the redundant `status()` per `wait_for_confirmation`) | ~3-4 RT/test | Low-Med |
| P6 | xdist beyond -n2: give each worker its own funded account (all workers currently sign with the SAME dispenser account — the -n>2 crash source). Potential 2–4×, but devmode's one-block-per-txn is a real ceiling — validate before investing | 2–4× ceiling-capped | High |

Do P1+P2+P3 first: independent, near-free. **LANDED 2026-07-08 (4542b9495d + conditional-reset
refinement); measured outcome:** variance COLLAPSED (637/643s back-to-back with reset; the 1013s
worst case is gone). A/B showed the reset itself costs ~100s (docker restart), so the auto-reset
became CONDITIONAL on ledger age (round >= 50k, ~6-7 runs of accumulation; REST probe, no docker
dep) — young-localnet full runs take the ~537s path, aged ones pay ~100s once to escape the drift.
Honest note: P2+P3's isolated gain vs the old best-case (419s) is not separable from OS-cache
noise at this scale; the durable wins are the variance kill + the bounded worst case.

---

## Part III — recommended sequence

1. **P1+P2+P3** (an afternoon; every subsequent gate run gets faster — compounding payoff).
2. **T2** (trivial carrier dedup, banks confidence) → **T4** (null-safety) → **R4/R5** (quick LOC wins).
3. **R1/D1 `AbiCodec`** — the convergent centerpiece; equality-gated migration; subsumes T5/R6-half.
4. **R2** generic walker → enables **S3** (AWST validator) cheaply.
5. **D3 NameGen** (determinism; prerequisite for any parallel-compile future) + **D7** (flag rename).
6. **T1** width-tier predicate, staged per-file.
7. **S1 storage layout** then **S2 calldata geometry** — the two failure-count levers (~26 of 43).
8. **D2 ReturnRewriter** once R1 exists (its wire-type computation becomes AbiCodec calls).
9. **S5** promote the fuzzer; **D5/R7** ctor-modifier convergence last (high risk, low urgency).

Standing gates unchanged: full suite vs baseline (fail-set identical or better), relevant
differential-fuzz axes per slice, no puya-fork changes, open bugs stay regular fails.
