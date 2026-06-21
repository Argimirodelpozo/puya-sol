# solc-reuse opportunities

Places where puya-sol re-derives a value / size / type that the vendored solc
(`libsolidity`) already computes exactly — and has gotten subtly wrong. Leaning on
solc's computed facts removes hand-rolled code *and* whole bug classes.

**The throughline (proven by the differential fuzzer):** nearly every value-level
divergence found this cycle — wide-array `.length`, `int8(-1)` non-canonical,
signed sub-word compare/equality — is puya recomputing something solc already has.
The fuzzer is effectively a detector for "haven't deferred to solc here yet."

---

## A. Use `ConstantEvaluator` / `literalValue()` for ALL constant subexpressions
**Status: highest value. Proven by the `int8(-1) == int8(-1)` bug (fixed 2026-06-21).**

> **Step 1 DONE (v420, 0bec5aed44):** `type(intN).min/max` now uses solc's `IntegerType::min()/max()`
> (256-bit TC u256) via a new shared `TypeCoercion::canonicalIntConstant(tcValue, bits)` — the single
> "solc value -> canonical int constant" rule. Deleted ~40 lines of hand-rolled TC math in
> SolMetaTypeAccess + the "lockstep with SolLiteral" coupling. Zero-reg, guard test_type_minmax_canonical.
> **Step 2 DONE (v421, 4c7f7032e3):** (a) SolLiteral's signed-small `%2^64` wrap branch turned out to be
> DEAD code (needed m_solType to be RationalNumberType AND IntegerType at once) — removed, verified dead by
> fuzz. (b) SolLiteral + tryConstantFold share the same magnitude rule but are width-less rationals, so they
> do NOT fit `canonicalIntConstant` (which needs a fixed width); extracted a separate small
> `TypeCoercion::rationalIntConstant` and routed both through it. Two constant-emission shapes now exist:
> width-based (`canonicalIntConstant`, for type(T).min/max + intN casts) and magnitude-based
> (`rationalIntConstant`, for rationals).
> **Step 3 (the const-fold gap) DONE — but it DID NOT EXIST (v422 guard, no compiler change):** investigated
> with the differential fuzzer + harness. `type(uint64).max**2` does NOT fold-and-widen on EVM — its type is
> `uint64`, so `(2^64-1)^2` overflows in checked context and reverts on EVM AND AVM identically. The old
> "gap" note was a misread: the fuzzer's "no divergence" for it was a BOTH-revert, not a value match.
> Constants that fit ARE folded correctly (`10**77`, `1<<200`, `2^255`, `3*2^200`), unchecked ones wrap in
> their operand width on both — all match EVM. So `ConstantEvaluator::evaluate()` integration buys nothing.
> Locked by guard `test_const_fold_arbitrary_precision`. **Opportunity A is now COMPLETE** (steps 1-2 were
> consolidation; step 3 was a non-issue). NB: SolLiteral was already canonical; the `int8(-1)` bug was the
> explicit-cast path only (SolTypeConversion) — A was consolidation, not bug-fixing.

Today `ConstantEvaluator` is only used in `SolInlineAssembly.cpp`. The high-level path
hand-rolls constants:
- `type(intN).min` is computed by hand in `SolMetaTypeAccess.cpp`.
- `SolLiteral.cpp` has a "wrap signed-small literals to 64-bit TC so comparisons line
  up" hack (line ~42-53).
- `int8(-1)` reached equality as a non-canonical `255` (cast masked, didn't sign-extend).

If constant subexpressions were folded via solc and **emitted in canonical form**:
- delete the hand-rolled `type(T).min/max` lowering + the SolLiteral wrap hack,
- close the `type(uint64).max ** 2` const-fold gap (currently a runtime revert vs EVM's
  arbitrary-precision fold),
- erase the non-canonical-constant bug class at the source.

Where: `SolLiteral.cpp`, `SolMetaTypeAccess.cpp`, `SolTypeConversion.cpp` (constant cast
fast-path). Reuse `Type::literalValue()` / `ConstantEvaluator::evaluate()`.

## B. Reuse solc's storage layout (`--storage-layout`: slot + offset + packed)
solc assigns every state var an exact `(slot, byteOffset, inStorage)` layout. The
EVM-faithful assembly `sload`/`sstore` path (`StorageDispatch`) recomputes slot math and
has had aliasing bugs (mod-256 computed-slot collisions; see silent-stub-sweep notes).
Adopt solc's computed slots as the source of truth for the asm-storage path instead of
parallel math. Caveat: puya's high-level state uses boxes, not EVM slots — this applies
to the assembly/EVM-faithful path, not box storage.

## C. `Type::storageBytes()` / `calldataEncodedSize(false)` for element & field sizes
**Investigated 2026-06-21: NOT viable as a swap, and no latent bug to catch.**
`computeEncodedElementSize` (`Arc4Defaults.cpp`) is **WType-based** (24 call sites, many of which
only have an ARC4 WType, not a Solidity `Type`), so solc's `calldataEncodedSize` (a Solidity-`Type`
method) can't replace it uniformly. And the sizes are **context-dependent**: BOX storage is packed
ARC4 (uint128 = 16B → `mapSolTypeToARC4`) while the MEMORY BLOB is 32-byte words (uint128 → 32B via
`map()`), so there's no single right answer per type. `bool` (puya 8B) and `address` (puya 32B account)
also genuinely differ from solc's ABI (1B / 20B) — by design. For integers the hand-rolled switch
already agrees with solc (`ARC4UIntN(n) → n/8`). The wide-array bug was a call-site input error (a
width-erased `map()` feeding the box path), already fixed; an audit of the other sites found them
correct-for-their-context (blob sites correctly use 32). Box + memory sub-word aggregates fuzzed CLEAN
vs live EVM; guard `memory_subword_aggregate` added for the memory path (was uncovered). Same shape as
the commonType finding: puya's type model (WType, box-vs-blob, bool/address widths) diverges from solc
at exactly these size seams — which is *why* puya has its own abstraction.

## D. `commonType` + `mobileType()` + the implicit-conversion lattice  ← TACKLING NEXT
solc annotates every `BinaryOperation` with `annotation().commonType` (the type both
operands are implicitly converted to before the op) and every expression with its exact
`annotation().type` / `mobileType()`. `TypeCoercion` reimplements numeric promotion and
literal-typing. Driving operand conversions from solc's `commonType` keeps puya consistent
with the exact rules the fuzzer keeps probing, and drops the parallel promotion logic.
Promising for the implicit operand casts inside binary ops / comparisons — where the
width/sign bugs live.

Where: `SolBinaryOperation.cpp`, `TypeCoercion.cpp` (implicitNumericCast / promotion).

**Investigated 2026-06-21 (comparisons):** net-additive, NOT a reduction. `compare()` has one
caller (`SolBinaryOperation.cpp:145`) and comparisons don't use `commonType` today — but its
per-operand promotion already handles mixed widths correctly, so coercing to `commonType` first
*duplicates* it rather than replacing it. The canonicalization (cast/equality sign-extend) must stay
regardless (commonType only handles cross-width; same-width non-canonical operands still need it).
`narrowConstIfNegative` (~25 lines) can't be removed because literal operands (`x == -128`) are
`RationalNumberType`, bypass an integer-typed coercion guard, and still reach `compare()` mismatched.
**Residual real win:** the *arithmetic* path (`SolBinaryOperation.cpp:170-178`) uses
`builderForInstance(commonType, _left)` which *reinterprets* the operand without converting the value
— a latent signed-widening gap (a non-canonical operand in `int8 + int16` zero-extends the sign).
Making it *coerce* via `signExtendSignedWiden` would be correct-by-construction. (Today's cast/equality
fixes plug it at the source, so it's latent, not active.)

## E. `FunctionType::externalSignature()` for canonical signature strings
Method *selectors* are ARC4 (different scheme), but the canonical-param-type-name string
construction — used for error/event encoding and the `intSelectorName`/
`arc4EncodeArgsAtParamTypes` width logic that had bugs (see sol-eb-audit selector-width) —
is something solc already builds correctly via `externalSignature()`.

## F. Model `Type::cleanupNeededForOp` — the principled "centralize canonicalization"
solc attaches, per type and per op, whether a value needs cleanup before use, decided
once. That's the disciplined form of collapsing puya's ~54 scattered `signExtend*` call
sites (across 19 files): attach a `canonicalize()` to each WType rather than remembering
it at every consumer. This session alone needed canonicalization added at the cast,
ordering-compare, and equality-compare sites — three places for one invariant.

## G. `type(I).interfaceId`, C3 linearization, overload resolution
solc computes interface IDs (XOR of selectors), `linearizedBaseContracts` (C3 MRO for
super/virtual dispatch), and overload resolution (`referencedDeclaration`). Reuse rather
than reimplement super/interface dispatch (likely already partly used via
`referencedDeclaration`).

---

### Priority
1. ~~**A** (ConstantEvaluator / canonical constants)~~ — **DONE** (v420-v422): type(T).min/max + SolLiteral
   dead-branch + shared canonicalIntConstant/rationalIntConstant; const-fold gap debunked (never existed).
2. ~~**D** (commonType for comparisons)~~ — investigated, net-additive (see note above); residual = the
   arith-path coerce-vs-reinterpret tweak, still open.
3. **C** (size from solc) and **B** (storage layout from solc) — next tier, untouched.
4. **E / F / G** — larger or partial-reuse refactors, untouched.
